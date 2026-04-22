import { Trend, Rate } from 'k6/metrics';
import {
  fastLookupSql,
  seedWorkload,
  slowScanSql,
  sqlRequest,
  mustSucceed,
} from './lib.js';

export const fastLookupDuration = new Trend('fast_lookup_duration', true);
export const slowScanDuration = new Trend('slow_scan_duration', true);
export const fastLookupSuccess = new Rate('fast_lookup_success');
export const slowScanSuccess = new Rate('slow_scan_success');

export const options = {
  scenarios: {
    slow_scan: {
      executor: 'constant-vus',
      vus: Number(__ENV.SLOW_VUS || 4),
      duration: __ENV.TEST_DURATION || '20s',
      exec: 'slowScan',
    },
    fast_lookup: {
      executor: 'constant-vus',
      vus: Number(__ENV.FAST_VUS || 4),
      duration: __ENV.TEST_DURATION || '20s',
      exec: 'fastLookup',
    },
  },
  thresholds: {
    fast_lookup_success: ['rate>0.99'],
    slow_scan_success: ['rate>0.99'],
    fast_lookup_duration: [`p(95)<${__ENV.FAST_P95_MS || 70}`],
    slow_scan_duration: ['avg>=0'],
  },
};

export function setup() {
  return seedWorkload();
}

export function slowScan(data) {
  const response = sqlRequest(
    slowScanSql(data.bigTable),
    { kind: 'slow_scan', name: 'slow_scan' }
  );
  const body = mustSucceed(response, 'slow_scan');

  slowScanDuration.add(response.timings.duration);
  slowScanSuccess.add(body.rowCount === 0);
}

export function fastLookup(data) {
  const response = sqlRequest(
    fastLookupSql(data.bigTable, data.bigRows),
    { kind: 'fast_lookup', name: 'fast_lookup' }
  );
  const body = mustSucceed(response, 'fast_lookup');

  fastLookupDuration.add(response.timings.duration);
  fastLookupSuccess.add(body.rowCount === 1);
}
