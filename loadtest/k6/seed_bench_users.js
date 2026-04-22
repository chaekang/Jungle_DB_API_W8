import { check } from 'k6';

import { envInt, envString, postSql } from './common.js';

const seedRows = envInt('SEED_ROWS', 10000);
const seedVus = envInt('SEED_VUS', 50);

export const options = {
  scenarios: {
    seed: {
      executor: 'shared-iterations',
      vus: seedVus,
      iterations: seedRows,
      maxDuration: envString('SEED_MAX_DURATION', '10m'),
    },
  },
  thresholds: {
    http_req_failed: ['rate==0'],
  },
};

export default function () {
  const sql =
    `INSERT INTO bench_users (name, age) VALUES ` +
    `('seed_${__VU}_${__ITER}', ${20 + ((__VU + __ITER) % 41)});`;

  const response = postSql(sql, { phase: 'seed' });
  check(response, {
    'seed status is 200': (res) => res.status === 200,
  });
}
