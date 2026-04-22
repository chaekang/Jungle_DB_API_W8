import { Trend, Rate } from 'k6/metrics';
import {
  fastLookupSql,
  seedWorkload,
  slowScanSql,
  sqlRequest,
  mustSucceed,
} from './lib.js';

export const isolatedLookupDuration = new Trend('isolated_lookup_duration', true);
export const slowScanDuration = new Trend('slow_scan_duration', true);
export const isolatedLookupSuccess = new Rate('isolated_lookup_success');
export const slowScanSuccess = new Rate('slow_scan_success');

export const options = {
  scenarios: {
    slow_scan_big_table: {
      executor: 'constant-vus',
      vus: Number(__ENV.SLOW_VUS || 4),
      duration: __ENV.TEST_DURATION || '20s',
      exec: 'slowScanBigTable',
    },
    fast_lookup_small_table: {
      executor: 'constant-vus',
      vus: Number(__ENV.FAST_VUS || 4),
      duration: __ENV.TEST_DURATION || '20s',
      exec: 'fastLookupSmallTable',
    },
  },
  thresholds: {
    isolated_lookup_success: ['rate>0.99'],
    slow_scan_success: ['rate>0.99'],
    isolated_lookup_duration: [`p(95)<${__ENV.FAST_P95_MS || 50}`],
    slow_scan_duration: ['avg>=0'],
  },
};

export function setup() {
  return seedWorkload();
}

export function slowScanBigTable(data) {
  const response = sqlRequest(
    slowScanSql(data.bigTable),
    { kind: 'slow_scan_big_table', name: 'slow_scan_big_table' }
  );
  const body = mustSucceed(response, 'slow_scan_big_table');

  slowScanDuration.add(response.timings.duration);
  slowScanSuccess.add(body.rowCount === 0);
}

export function fastLookupSmallTable(data) {
  const response = sqlRequest(
    fastLookupSql(data.smallTable, data.smallRows),
    { kind: 'isolated_lookup', name: 'isolated_lookup' }
  );
  const body = mustSucceed(response, 'isolated_lookup');

  isolatedLookupDuration.add(response.timings.duration);
  isolatedLookupSuccess.add(body.rowCount === 1);
}
