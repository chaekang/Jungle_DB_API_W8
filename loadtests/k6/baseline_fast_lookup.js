import { Trend, Rate } from 'k6/metrics';
import {
  fastLookupSql,
  seedWorkload,
  sqlRequest,
  mustSucceed,
} from './lib.js';

export const fastLookupDuration = new Trend('fast_lookup_duration', true);
export const fastLookupSuccess = new Rate('fast_lookup_success');

export const options = {
  vus: Number(__ENV.FAST_VUS || 8),
  duration: __ENV.TEST_DURATION || '20s',
  thresholds: {
    fast_lookup_success: ['rate>0.99'],
    fast_lookup_duration: [`p(95)<${__ENV.FAST_P95_MS || 50}`],
  },
};

export function setup() {
  return seedWorkload();
}

export default function (data) {
  const response = sqlRequest(
    fastLookupSql(data.bigTable, data.bigRows),
    { kind: 'fast_lookup', name: 'fast_lookup' }
  );
  const body = mustSucceed(response, 'fast_lookup');

  fastLookupDuration.add(response.timings.duration);
  fastLookupSuccess.add(body.rowCount === 1);
}

