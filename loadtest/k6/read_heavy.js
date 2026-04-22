import { check } from 'k6';
import { Counter, Trend } from 'k6/metrics';

import { envInt, envString, postSql, randomInt } from './common.js';

const arrivalRate = envInt('ARRIVAL_RATE', 200);
const preAllocatedVus = envInt('PRE_ALLOCATED_VUS', 200);
const maxVus = envInt('MAX_VUS', 1000);
const selectIdMax = envInt('SELECT_ID_MAX', 10000);

const status503 = new Counter('status_503');
const successDuration = new Trend('success_duration');

export const options = {
  scenarios: {
    read_heavy: {
      executor: 'constant-arrival-rate',
      rate: arrivalRate,
      timeUnit: '1s',
      duration: envString('TEST_DURATION', '30s'),
      preAllocatedVUs: preAllocatedVus,
      maxVUs: maxVus,
    },
  },
};

export default function () {
  const sql = `SELECT * FROM bench_users WHERE id = ${randomInt(1, selectIdMax)};`;
  const response = postSql(sql, {
    workload: 'read-heavy',
    sql_type: 'select',
  });

  if (response.status === 503) {
    status503.add(1);
  }
  if (response.status === 200) {
    successDuration.add(response.timings.duration);
  }

  check(response, {
    'status is 200 or 503': (res) => res.status === 200 || res.status === 503,
  });
}
