import http from 'k6/http';

export function envInt(name, fallback) {
  const value = __ENV[name];
  if (value === undefined || value === '') {
    return fallback;
  }

  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    return fallback;
  }

  return parsed;
}

export function envString(name, fallback) {
  const value = __ENV[name];
  if (value === undefined || value === '') {
    return fallback;
  }

  return value;
}

export function randomInt(min, max) {
  return Math.floor(Math.random() * (max - min + 1)) + min;
}

export function postSql(sql, tags = {}) {
  const baseUrl = envString('BASE_URL', 'http://127.0.0.1:8080');

  return http.post(
    `${baseUrl}/query`,
    JSON.stringify({ sql }),
    {
      headers: { 'Content-Type': 'application/json' },
      tags,
    }
  );
}
