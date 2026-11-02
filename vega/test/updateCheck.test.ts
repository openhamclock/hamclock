import {fetchLatestVersion, isNewer, noticeFor, parseVersion, versionUrls} from '../src/updateCheck';

describe('parseVersion', () => {
  it('parses release and beta forms', () => {
    expect(parseVersion('4.32')).toEqual({major: 4, minor: 32, patch: 0, beta: null});
    expect(parseVersion('v4.32.1')).toEqual({major: 4, minor: 32, patch: 1, beta: null});
    expect(parseVersion('4.34b00')).toEqual({major: 4, minor: 34, patch: 0, beta: 0});
    expect(parseVersion('v4.32b01.0')).toEqual({major: 4, minor: 32, patch: 0, beta: 1});
  });
  it('rejects edge and junk', () => {
    expect(parseVersion('edge')).toBeNull();
    expect(parseVersion('')).toBeNull();
    expect(parseVersion(undefined)).toBeNull();
    expect(parseVersion('Changes with latest release:')).toBeNull();
  });
});

describe('isNewer (Android VersionComparator rules)', () => {
  it('compares major/minor/patch', () => {
    expect(isNewer('4.33', '4.34')).toBe(true);
    expect(isNewer('4.34', '4.33')).toBe(false);
    expect(isNewer('4.34', '4.34')).toBe(false);
    expect(isNewer('4.34', '5.0')).toBe(true);
    expect(isNewer('4.34.0', '4.34.1')).toBe(true);
  });
  it('never offers a beta to a stable install', () => {
    expect(isNewer('4.33', '4.34b01')).toBe(false);
  });
  it('offers betas the final release and later betas', () => {
    expect(isNewer('4.34b00', '4.34')).toBe(true);
    expect(isNewer('4.34b00', '4.34b01')).toBe(true);
    expect(isNewer('4.34b01', '4.34b00')).toBe(false);
    expect(isNewer('4.34b99', '4.33')).toBe(false);
  });
  it('is false when either side is unknown', () => {
    expect(isNewer('edge', '4.40')).toBe(false);
    expect(isNewer('4.34', '')).toBe(false);
  });
});

describe('versionUrls', () => {
  it('tries HTTPS on the host, then the backend as set, then the default', () => {
    expect(versionUrls('ohb.hamclock.app:80')).toEqual([
      'https://ohb.hamclock.app/ham/HamClock/version.pl',
      'http://ohb.hamclock.app:80/ham/HamClock/version.pl',
    ]);
    expect(versionUrls('my.host:8000')).toEqual([
      'https://my.host/ham/HamClock/version.pl',
      'http://my.host:8000/ham/HamClock/version.pl',
      'https://ohb.hamclock.app/ham/HamClock/version.pl',
    ]);
  });
});

describe('fetchLatestVersion', () => {
  const res = (status: number, body: string) =>
    ({ok: status >= 200 && status < 300, status, text: async () => body}) as never;

  it('returns the first line of the first answer', async () => {
    const fetchFn = jest.fn(async () => res(200, '4.35\r\nChanges with latest release:\n- x\n'));
    expect(await fetchLatestVersion('ohb.hamclock.app:80', 'HamClock-vega/4.34', fetchFn as never)).toBe('4.35');
    expect(fetchFn).toHaveBeenCalledTimes(1);
    expect((fetchFn.mock.calls[0] as unknown[])[1]).toMatchObject({headers: {'User-Agent': 'HamClock-vega/4.34'}});
  });
  it('falls through errors and bad answers', async () => {
    const fetchFn = jest
      .fn()
      .mockRejectedValueOnce(new Error('offline'))
      .mockResolvedValueOnce(res(404, 'nope'))
      .mockResolvedValueOnce(res(200, '4.36\n'));
    expect(await fetchLatestVersion('my.host:8000', 'ua', fetchFn as never)).toBe('4.36');
    expect(fetchFn).toHaveBeenCalledTimes(3);
  });
  it('returns null if nothing answers', async () => {
    const fetchFn = jest.fn(async () => res(500, ''));
    expect(await fetchLatestVersion('ohb.hamclock.app:80', 'ua', fetchFn as never)).toBeNull();
  });
});

describe('noticeFor', () => {
  it('shows a newer version unless snoozed for it', () => {
    expect(noticeFor('4.34', '4.35', null, 0)).toBe('4.35');
    expect(noticeFor('4.34', '4.34', null, 0)).toBeNull();
    expect(noticeFor('4.34', '4.35', {version: '4.35', until: 100}, 50)).toBeNull();
    expect(noticeFor('4.34', '4.35', {version: '4.35', until: 100}, 150)).toBe('4.35');
    expect(noticeFor('4.34', '4.36', {version: '4.35', until: 100}, 50)).toBe('4.36');
  });
});
