import { Counter, Rate, Trend } from 'k6/metrics';
import {
  fastLookupSql,
  seedWorkload,
  slowScanSql,
  sqlRequest,
  mustSucceed,
  decodeBody,
} from './lib.js';

export const slowScanDuration = new Trend('slow_scan_duration', true);
export const burstProbeDuration = new Trend('burst_probe_duration', true);
export const slowScanSuccess = new Rate('slow_scan_success');
export const burstSuccess = new Rate('burst_success');
export const queueRejected = new Counter('queue_rejected');
export const queueAccepted = new Counter('queue_accepted');

export const options = {
  scenarios: {
    slow_scan_holders: {
      executor: 'constant-vus',
      vus: Number(__ENV.SLOW_VUS || 8),
      duration: __ENV.TEST_DURATION || '20s',
      exec: 'slowScanHolder',
    },
    burst_probe: {
      executor: 'constant-arrival-rate',
      rate: Number(__ENV.OVERLOAD_RATE || 120),
      timeUnit: '1s',
      duration: __ENV.BURST_DURATION || '10s',
      preAllocatedVUs: Number(__ENV.BURST_PREALLOCATED_VUS || 64),
      maxVUs: Number(__ENV.BURST_MAX_VUS || 256),
      startTime: __ENV.BURST_START || '2s',
      exec: 'burstProbe',
    },
  },
  thresholds: {
    slow_scan_success: ['rate>0.99'],
    burst_success: ['rate>=0'],
    queue_rejected: ['count>=0'],
    queue_accepted: ['count>=0'],
    burst_probe_duration: ['avg>=0'],
  },
};

export function setup() {
  return seedWorkload();
}

export function slowScanHolder(data) {
  const response = sqlRequest(
    slowScanSql(data.bigTable),
    { kind: 'slow_scan_holder', name: 'slow_scan_holder' }
  );
  const body = mustSucceed(response, 'slow_scan_holder');

  slowScanDuration.add(response.timings.duration);
  slowScanSuccess.add(body.rowCount === 0);
}

export function burstProbe(data) {
  const response = sqlRequest(
    fastLookupSql(data.bigTable, data.bigRows),
    { kind: 'burst_probe', name: 'burst_probe' }
  );
  const body = decodeBody(response);

  burstProbeDuration.add(response.timings.duration);
  if (response.status === 503) {
    queueRejected.add(1);
    burstSuccess.add(body !== null && body.success === false);
    return;
  }

  if (response.status === 200 && body !== null && body.success === true) {
    queueAccepted.add(1);
    burstSuccess.add(body.rowCount === 1);
    return;
  }

  burstSuccess.add(false);
}
