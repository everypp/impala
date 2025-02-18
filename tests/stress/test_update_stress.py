# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

from __future__ import absolute_import, division, print_function
from builtins import map, range
import pytest
import random
import time

from tests.common.impala_test_suite import ImpalaTestSuite
from tests.common.parametrize import UniqueDatabase
from tests.common.test_vector import ImpalaTestDimension
from tests.stress.stress_util import run_tasks, Task
from tests.util.filesystem_utils import IS_HDFS


# Longer-running UPDATE tests are executed here
class TestIcebergV2UpdateStress(ImpalaTestSuite):
  """UPDATE tests against Iceberg V2 tables."""

  BATCH_SIZES = [0, 32]
  EXHAUSTIVE_BATCH_SIZES = [0, 1, 11, 32]

  @classmethod
  def get_workload(cls):
    return 'functional-query'

  @classmethod
  def add_test_dimensions(cls):
    super(TestIcebergV2UpdateStress, cls).add_test_dimensions()
    cls.ImpalaTestMatrix.add_constraint(
      lambda v: v.get_value('table_format').file_format == 'parquet')
    if cls.exploration_strategy() == 'core':
      cls.ImpalaTestMatrix.add_dimension(
        ImpalaTestDimension('batch_size', *TestIcebergV2UpdateStress.BATCH_SIZES))
    else:
      cls.ImpalaTestMatrix.add_dimension(
        ImpalaTestDimension('batch_size',
            *TestIcebergV2UpdateStress.EXHAUSTIVE_BATCH_SIZES))

  def test_update_stress(self, vector, unique_database):
    self.run_test_case('QueryTest/iceberg-update-stress', vector,
        unique_database)
    if IS_HDFS:
      self._update_stress_hive_tests(unique_database)

  def _update_stress_hive_tests(self, db):
    stmt = """
        SELECT count(*), sum(ss_ticket_number)
        FROM {}.ice_store_sales
        WHERE ss_item_sk % 1999 = 0""".format(db)

    hive_results = self.run_stmt_in_hive(stmt).split("\n", 1)[1]
    assert hive_results == \
        "3138,848464922\n"


# Stress test for concurrent UPDATE operations against Iceberg tables.
class TestIcebergConcurrentUpdateStress(ImpalaTestSuite):
  @classmethod
  def get_workload(self):
    return 'targeted-stress'

  @classmethod
  def add_test_dimensions(cls):
    super(TestIcebergConcurrentUpdateStress, cls).add_test_dimensions()
    cls.ImpalaTestMatrix.add_constraint(
        lambda v: (v.get_value('table_format').file_format == 'parquet'
            and v.get_value('table_format').compression_codec == 'snappy'))

  def _impala_role_concurrent_writer(self, tbl_name, col, num_updates):
    """Increments values in column 'total' and in the column which is passed in 'col'."""
    target_impalad = random.randint(0, ImpalaTestSuite.get_impalad_cluster_size() - 1)
    impalad_client = ImpalaTestSuite.create_client_for_nth_impalad(target_impalad)
    update_cnt = 0
    while update_cnt < num_updates:
      try:
        impalad_client.execute(
            "update {0} set total = total + 1, {1} = {1} + 1".format(tbl_name, col))
        update_cnt += 1
        # Sleep after a succesful operation.
        time.sleep(random.random())
      except Exception:
        # Exceptions are expected due to concurrent operations.
        pass
    impalad_client.close()

  def _impala_role_concurrent_checker(self, tbl_name, target_total):
    """Checks if the table's invariant is true. The invariant is that the equation
    'total == a + b + c' is true. Returns 'total'."""
    def verify_result_set(result):
      assert len(result.data) == 1
      line = result.data[0]
      [total, a, b, c] = list(map(int, (line.split('\t'))))
      assert total == a + b + c
      return total

    target_impalad = random.randint(0, ImpalaTestSuite.get_impalad_cluster_size() - 1)
    impalad_client = ImpalaTestSuite.create_client_for_nth_impalad(target_impalad)
    total = 0
    while total < target_total:
      result = impalad_client.execute("select * from %s" % tbl_name)
      new_total = verify_result_set(result)
      assert total <= new_total
      total = new_total
      time.sleep(random.random())
    impalad_client.close()

  @pytest.mark.stress
  @UniqueDatabase.parametrize(sync_ddl=True)
  def test_iceberg_updates(self, unique_database):
    """Issues UPDATE statements against multiple impalads in a way that some
    invariants must be true when a spectator process inspects the table. E.g.
    the value of a column should be equal to the sum of other columns."""
    tbl_name = "%s.test_concurrent_updates" % unique_database
    self.client.set_configuration_option("SYNC_DDL", "true")
    self.client.execute("""create table {0}
        (total bigint, a bigint, b bigint, c bigint)
        stored as iceberg
        tblproperties('format-version'='2')""".format(tbl_name,))
    self.client.execute(
        "insert into {} values (0, 0, 0, 0)".format(tbl_name))

    num_checkers = 2
    cols = 3
    updates_per_col = 30
    target_total = updates_per_col * cols

    updater_a = Task(self._impala_role_concurrent_writer, tbl_name, "a", updates_per_col)
    updater_b = Task(self._impala_role_concurrent_writer, tbl_name, "b", updates_per_col)
    updater_c = Task(self._impala_role_concurrent_writer, tbl_name, "c", updates_per_col)
    checkers = [Task(self._impala_role_concurrent_checker, tbl_name, target_total)
                for i in range(0, num_checkers)]
    run_tasks([updater_a, updater_b, updater_c] + checkers)
