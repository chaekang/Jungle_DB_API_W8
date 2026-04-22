import { Trend, Rate } from 'k6/metrics';
import {
  seedWorkload,
  slowScanSql,
  writerInsertSql,
  sqlRequest,
  mustSucceed,
} from './lib.js';

export const slowScanDuration = new Trend('slow_scan_duration', true);
export const writerDuration = new Trend('writer_duration', true);
export const slowScanSuccess = new Rate('slow_scan_success');
export const writerSuccess = new Rate('writer_success');

export const options = {
  scenarios: {
    slow_scan: {
      executor: 'constant-vus',
      vus: Number(__ENV.SLOW_VUS || 4),
      duration: __ENV.TEST_DURATION || '20s',
      exec: 'slowScan',
    },
    writers: {
      executor: 'constant-vus',
      vus: Number(__ENV.WRITER_VUS || 2),
      duration: __ENV.TEST_DURATION || '20s',
      exec: 'writer',
    },
  },
  thresholds: {
    slow_scan_success: ['rate>0.99'],
    writer_success: ['rate>0.99'],
    slow_scan_duration: ['avg>=0'],
    writer_duration: ['avg>=0'],
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

export function writer(data) {
  const response = sqlRequest(
    writerInsertSql(data.bigTable),
    { kind: 'writer', name: 'writer' }
  );
  const body = mustSucceed(response, 'writer');

  writerDuration.add(response.timings.duration);
  writerSuccess.add(body.message !== undefined);
}
