// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/iceberg-delete-sink-base.h"

#include <boost/algorithm/string.hpp>

#include "exec/iceberg-delete-sink-config.h"
#include "exec/output-partition.h"
#include "exprs/scalar-expr-evaluator.h"
#include "kudu/util/url-coding.h"
#include "util/debug-util.h"
#include "util/iceberg-utility-functions.h"

#include "common/names.h"

namespace impala {

IcebergDeleteSinkBase::IcebergDeleteSinkBase(TDataSinkId sink_id,
    const IcebergDeleteSinkConfig& sink_config, const std::string& name,
    RuntimeState* state) :
    TableSinkBase(sink_id, sink_config, name, state) {
}

Status IcebergDeleteSinkBase::Prepare(RuntimeState* state,
    MemTracker* parent_mem_tracker) {
  RETURN_IF_ERROR(TableSinkBase::Prepare(state, parent_mem_tracker));
  unique_id_str_ = "delete-" + PrintId(state->fragment_instance_id(), "-");

  // Resolve table id and set input tuple descriptor.
  table_desc_ = static_cast<const HdfsTableDescriptor*>(
      state->desc_tbl().GetTableDescriptor(table_id_));
  if (table_desc_ == nullptr) {
    stringstream error_msg("Failed to get table descriptor for table id: ");
    error_msg << table_id_;
    return Status(error_msg.str());
  }

  DCHECK_GE(output_expr_evals_.size(),
      table_desc_->num_cols() - table_desc_->num_clustering_cols()) << DebugString();

  return Status::OK();
}

Status IcebergDeleteSinkBase::Open(RuntimeState* state) {
  RETURN_IF_ERROR(TableSinkBase::Open(state));
  DCHECK_EQ(partition_key_expr_evals_.size(), dynamic_partition_key_expr_evals_.size());
  return Status::OK();
}

std::string IcebergDeleteSinkBase::HumanReadablePartitionValue(
    TIcebergPartitionTransformType::type transform_type, const std::string& value,
    Status* transform_result) {
  if (!iceberg::IsTimeBasedPartition(transform_type) ||
    value == table_desc_->null_partition_key_value()) {
    *transform_result = Status::OK();
    return value;
  }
  return iceberg::HumanReadableTime(transform_type, value, transform_result);
}

Status IcebergDeleteSinkBase::ConstructPartitionInfo(
    const TupleRow* row, OutputPartition* output_partition) {
  DCHECK(output_partition != nullptr);
  DCHECK(output_partition->raw_partition_names.empty());

  if (partition_key_expr_evals_.empty()) {
    output_partition->iceberg_spec_id = table_desc_->IcebergSpecId();
    return Status::OK();
  }

  DCHECK_EQ(partition_key_expr_evals_.size(), 2);

  ScalarExprEvaluator* spec_id_eval = partition_key_expr_evals_[0];
  ScalarExprEvaluator* partition_values_eval = partition_key_expr_evals_[1];

  int spec_id = spec_id_eval->GetIntVal(row).val;

  impala_udf::StringVal partition_values_sval = partition_values_eval->GetStringVal(row);
  string partition_values_str((const char*)partition_values_sval.ptr,
      partition_values_sval.len);
  return ConstructPartitionInfo(spec_id, partition_values_str, output_partition);
}

Status IcebergDeleteSinkBase::ConstructPartitionInfo(int32_t spec_id,
    const std::string& partition_values_str, OutputPartition* output_partition) {
  if (partition_key_expr_evals_.empty()) {
    DCHECK_EQ(spec_id, table_desc_->IcebergSpecId());
    output_partition->iceberg_spec_id = spec_id;
    return Status::OK();
  }
  output_partition->iceberg_spec_id = spec_id;

  vector<string> non_void_partition_names;
  vector<TIcebergPartitionTransformType::type> non_void_partition_transforms;
  if (LIKELY(spec_id == table_desc_->IcebergSpecId())) {
    // If 'spec_id' is the default spec id, then just copy the already populated
    // non void partition names and transforms.
    non_void_partition_names = table_desc_->IcebergNonVoidPartitionNames();
    non_void_partition_transforms = table_desc_->IcebergNonVoidPartitionTransforms();
  } else {
    // Otherwise collect the non-void partition names belonging to 'spec_id'.
    const TIcebergPartitionSpec& partition_spec =
        table_desc_->IcebergPartitionSpecs()[spec_id];
    for (const TIcebergPartitionField& spec_field : partition_spec.partition_fields) {
      auto transform_type = spec_field.transform.transform_type;
      if (transform_type != TIcebergPartitionTransformType::VOID) {
        non_void_partition_names.push_back(spec_field.field_name);
        non_void_partition_transforms.push_back(transform_type);
      }
    }
  }

  if (non_void_partition_names.empty()) {
    DCHECK(partition_values_str.empty());
    return Status::OK();
  }

  vector<string> partition_values_encoded;
  boost::split(partition_values_encoded, partition_values_str, boost::is_any_of("."));
  vector<string> partition_values_decoded;
  partition_values_decoded.reserve(partition_values_encoded.size());
  for (const string& encoded_part_val : partition_values_encoded) {
    string decoded_val;
    bool success = kudu::Base64Decode(encoded_part_val, &decoded_val);
    // We encoded it, we must succeed decoding it.
    DCHECK(success);
    partition_values_decoded.push_back(std::move(decoded_val));
  }

  DCHECK_EQ(partition_values_decoded.size(), non_void_partition_names.size());
  DCHECK_EQ(partition_values_decoded.size(), non_void_partition_transforms.size());

  stringstream url_encoded_partition_name_ss;

  for (int i = 0; i < partition_values_decoded.size(); ++i) {
    auto transform_type = non_void_partition_transforms[i];
    stringstream raw_partition_key_value_ss;
    stringstream url_encoded_partition_key_value_ss;

    raw_partition_key_value_ss << non_void_partition_names[i] << "=";
    url_encoded_partition_key_value_ss << non_void_partition_names[i] << "=";

    string& value_str = partition_values_decoded[i];
    Status transform_status;
    value_str = HumanReadablePartitionValue(
        transform_type, value_str, &transform_status);
    if (!transform_status.ok()) return transform_status;
    raw_partition_key_value_ss << value_str;

    string part_key_value = UrlEncodePartitionValue(value_str);
    url_encoded_partition_key_value_ss << part_key_value;
    if (i < partition_values_decoded.size() - 1) {
      url_encoded_partition_key_value_ss << "/";
    }
    url_encoded_partition_name_ss << url_encoded_partition_key_value_ss.str();
    output_partition->raw_partition_names.push_back(raw_partition_key_value_ss.str());
  }

  output_partition->partition_name = url_encoded_partition_name_ss.str();

  return Status::OK();
}

}
