import {FETCH_TIMEOUT_MS, pollOnce, requestHeaders, runFetch} from '../src/engine/fetchBridge';
import type {FetchBridgeDeps, FetchEngine} from '../src/engine/fetchBridge';
import type {FetchRequest} from '../src/engine';

function req(over: Partial<FetchRequest> = {}): FetchRequest {
  return {id: 1, url: 'https://hams.at/api/alerts/upcoming', userAgent: 'HamClock-vega/4.34', header: '', ...over};
}

function engine(queue: FetchRequest[] = []) {
  const done: {id: number; body: string | null}[] = [];
  const e: FetchEngine = {
    takeFetches: () => queue.splice(0),
    fetchDone: (id, body) => done.push({id, body}),
  };
  return {e, done};
}

function response(status: number, body: string): Response {
  return {status, text: async () => body} as unknown as Response;
}

describe('requestHeaders', () => {
  it('maps curl -A and -H', () => {
    expect(requestHeaders(req({header: 'Authorization: Bearer abc:def'}))).toEqual({
      'User-Agent': 'HamClock-vega/4.34',
      Authorization: 'Bearer abc:def',
    });
  });

  it('leaves out empty or malformed values', () => {
    expect(requestHeaders(req({userAgent: '', header: 'no colon'}))).toEqual({});
    expect(requestHeaders(req({userAgent: '', header: ': x'}))).toEqual({});
  });
});

describe('runFetch', () => {
  let warn: jest.SpyInstance;
  beforeEach(() => {
    warn = jest.spyOn(console, 'warn').mockImplementation(() => {});
  });
  afterEach(() => warn.mockRestore());

  it('passes the body back with the request headers', async () => {
    const {e, done} = engine();
    const fetchFn = jest.fn(async () => response(200, '{"data":[]}'));
    await runFetch(req({header: 'Authorization: Bearer k'}), {engine: e, fetchFn: fetchFn as unknown as typeof fetch});
    expect(done).toEqual([{id: 1, body: '{"data":[]}'}]);
    const [url, init] = fetchFn.mock.calls[0] as unknown as [string, RequestInit];
    expect(url).toBe('https://hams.at/api/alerts/upcoming');
    expect(init.headers).toEqual({'User-Agent': 'HamClock-vega/4.34', Authorization: 'Bearer k'});
  });

  it('passes the body back for HTTP errors too (curl --silent without -f)', async () => {
    const {e, done} = engine();
    const fetchFn = async () => response(500, '{"message":"Something went wrong"}');
    await runFetch(req(), {engine: e, fetchFn: fetchFn as unknown as typeof fetch});
    expect(done).toEqual([{id: 1, body: '{"message":"Something went wrong"}'}]);
  });

  it('reports a network error as a failure', async () => {
    const {e, done} = engine();
    const fetchFn = async () => {
      throw new TypeError('Network request failed');
    };
    await runFetch(req(), {engine: e, fetchFn: fetchFn as unknown as typeof fetch});
    expect(done).toEqual([{id: 1, body: null}]);
    expect(warn).toHaveBeenCalled();
  });

  it('refuses other schemes without fetching', async () => {
    const {e, done} = engine();
    const fetchFn = jest.fn();
    await runFetch(req({url: 'file:///etc/passwd'}), {engine: e, fetchFn: fetchFn as unknown as typeof fetch});
    expect(fetchFn).not.toHaveBeenCalled();
    expect(done).toEqual([{id: 1, body: null}]);
  });

  it('aborts after the curl timeout', async () => {
    jest.useFakeTimers();
    try {
      const {e, done} = engine();
      const fetchFn = (_u: string, init: RequestInit) =>
        new Promise<Response>((_res, rej) => init.signal?.addEventListener('abort', () => rej(new Error('Aborted'))));
      const p = runFetch(req(), {engine: e, fetchFn: fetchFn as unknown as typeof fetch});
      jest.advanceTimersByTime(FETCH_TIMEOUT_MS);
      await p;
      expect(done).toEqual([{id: 1, body: null}]);
    } finally {
      jest.useRealTimers();
    }
  });
});

describe('pollOnce', () => {
  it('starts every new request once', async () => {
    const {e, done} = engine([req({id: 1}), req({id: 2, url: 'https://meshtastic.liamcottle.net/api/v1/nodes/1'})]);
    const deps: FetchBridgeDeps = {engine: e, fetchFn: (async () => response(200, 'x')) as unknown as typeof fetch};
    await Promise.all(pollOnce(deps));
    expect(pollOnce(deps)).toHaveLength(0);
    expect(done.map(d => d.id).sort()).toEqual([1, 2]);
  });

  it('survives a failing native call', () => {
    const warn = jest.spyOn(console, 'warn').mockImplementation(() => {});
    const e: FetchEngine = {
      takeFetches: () => {
        throw new Error('boom');
      },
      fetchDone: () => {},
    };
    expect(pollOnce({engine: e, fetchFn: fetch})).toEqual([]);
    warn.mockRestore();
  });
});

