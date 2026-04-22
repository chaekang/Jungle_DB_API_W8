import { check } from 'k6';
import { Counter, Trend } from 'k6/metrics';

import { envInt, envString, postSql, randomInt } from './common.js';

const arrivalRate = envInt('ARRIVAL_RATE', 200);
const preAllocatedVus = envInt('PRE_ALLOCATED_VUS', 200);
const maxVus = envInt('MAX_VUS', 1000);
const selectIdMax = envInt('SELECT_ID_MAX', 10000);
const insertRatio = Number(__ENV.INSERT_RATIO || '0.2');

const status503 = new Counter('status_503');
const successDuration = new Trend('success_duration');

export const options = {
  scenarios: {
    mixed: {
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
  const doInsert = Math.random() < insertRatio;
  const sql = doInsert
    ? `INSERT INTO bench_users (name, age) VALUES ('load_${__VU}_${__ITER}', ${randomInt(20, 60)});`
    : `SELECT * FROM bench_users WHERE id = ${randomInt(1, selectIdMax)};`;

  const response = postSql(sql, {
    workload: 'mixed',
    sql_type: doInsert ? 'insert' : 'select',
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
