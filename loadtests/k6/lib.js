import http from 'k6/http';
import exec from 'k6/execution';
import { check, fail } from 'k6';

export const baseUrl = __ENV.BASE_URL || 'http://127.0.0.1:8080';

export function makeTableName(prefix) {
  const timestamp = Date.now().toString(36);
  const randomPart = Math.floor(Math.random() * 1e9).toString(36);
  return `${prefix}_${timestamp}_${randomPart}`;
}

export function sqlRequest(sql, tags = {}) {
  return http.post(
    `${baseUrl}/query`,
    JSON.stringify({ sql }),
    {
      headers: { 'Content-Type': 'application/json' },
      tags,
      timeout: __ENV.K6_HTTP_TIMEOUT || '30s',
    }
  );
}

export function decodeBody(response) {
  try {
    return response.json();
  } catch (error) {
    return null;
  }
}

export function mustSucceed(response, contextLabel) {
  const body = decodeBody(response);
  const label = contextLabel || 'query';
  const ok =
    check(response, {
      [`${label}: status is 200`]: (res) => res.status === 200,
      [`${label}: response is valid JSON`]: () => body !== null,
      [`${label}: success is true`]: () => body !== null && body.success === true,
    });

  if (!ok) {
    fail(`${label} failed with status=${response.status} body=${response.body}`);
  }

  return body;
}

export function buildInsertSql(tableName, rowNumber, prefix = 'user') {
  const age = 20 + (rowNumber % 50);
  return `INSERT INTO ${tableName} (name, age) VALUES ('${prefix}_${rowNumber}', ${age});`;
}

export function seedTable(tableName, rowCount, prefix = 'user') {
  let rowNumber;

  for (rowNumber = 1; rowNumber <= rowCount; rowNumber++) {
    const response = sqlRequest(
      buildInsertSql(tableName, rowNumber, prefix),
      { kind: 'seed_insert', name: 'seed_insert' }
    );
    mustSucceed(response, `seed ${tableName} row ${rowNumber}`);
  }
}

export function seedWorkload() {
  const bigRows = Number(__ENV.BIG_ROWS || 5000);
  const smallRows = Number(__ENV.SMALL_ROWS || 100);
  const bigTable = makeTableName('k6_big');
  const smallTable = makeTableName('k6_small');

  seedTable(bigTable, bigRows, 'big');
  seedTable(smallTable, smallRows, 'small');

  return {
    bigTable,
    bigRows,
    smallTable,
    smallRows,
  };
}

export function randomId(maxValue) {
  return Math.floor(Math.random() * maxValue) + 1;
}

export function fastLookupSql(tableName, maxId) {
  return `SELECT id, name, age FROM ${tableName} WHERE id = ${randomId(maxId)};`;
}

export function slowScanSql(tableName) {
  return `SELECT id FROM ${tableName} WHERE age = -1;`;
}

export function writerInsertSql(tableName) {
  const iteration = exec.scenario.iterationInTest;
  const vu = exec.vu.idInTest;
  const age = 30 + (iteration % 40);
  return `INSERT INTO ${tableName} (name, age) VALUES ('writer_${vu}_${iteration}', ${age});`;
}

