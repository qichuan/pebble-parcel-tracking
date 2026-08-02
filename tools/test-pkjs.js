// Runs watch/src/pkjs/index.js against stubbed Ship24 responses and checks the
// packed strings it hands the watch. The byte budgets asserted here are the
// contract with the fixed-size buffers in watch/src/c/*.c — if you widen one,
// widen the other.
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const SRC = path.join(__dirname, '..', 'watch', 'src', 'pkjs', 'index.js');
const US = '\x1f';

let failures = 0;
function check(name, cond, extra) {
  console.log((cond ? 'PASS  ' : 'FAIL  ') + name + (cond ? '' : '  -> ' + extra));
  if (!cond) failures++;
}
function utf8Len(s) { return Buffer.byteLength(s, 'utf8'); }

// Buffer sizes in the C code; a field must fit with room for the NUL.
const C_BUFFERS = {
  nickname: 40,   // main.c ParcelRow.nickname
  label: 36,      // main.c ParcelRow.label
  ago: 16,        // main.c ParcelRow.ago
  description: 164, // timeline_window.c EventRow.description
  location: 44,   // timeline_window.c EventRow.location
  when: 20,       // timeline_window.c EventRow.when
};

// Boots a fresh pkjs instance with its own storage and a scripted Ship24.
function boot(respond) {
  const store = {};
  const sent = [];
  const listeners = {};
  const pending = [];

  const sandbox = {
    console: { log: () => {} },
    localStorage: {
      getItem: (k) => (k in store ? store[k] : null),
      setItem: (k, v) => { store[k] = String(v); },
      removeItem: (k) => { delete store[k]; },
    },
    Pebble: {
      addEventListener: (name, fn) => { (listeners[name] = listeners[name] || []).push(fn); },
      sendAppMessage: (msg) => { sent.push(msg); },
      openURL: (url) => { sandbox.__openedUrl = url; },
    },
    XMLHttpRequest: function () {
      this.open = (m, u) => { this._m = m; this._u = u; };
      this.setRequestHeader = () => {};
      this.send = (body) => {
        // Queue rather than fire: pkjs relies on requests completing async.
        pending.push(() => {
          const r = respond(this._m, this._u, body);
          this.status = r.status;
          this.responseText = JSON.stringify(r.body === undefined ? {} : r.body);
          if (r.status === 0) this.onerror(); else this.onload();
        });
      };
    },
    setTimeout: (fn) => { pending.push(fn); },
  };
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(SRC, 'utf8'), sandbox, { filename: SRC });

  // Drains queued work until quiet — pkjs chains requests one after another.
  function settle() {
    for (let i = 0; i < 200 && pending.length; i++) pending.shift()();
  }
  return {
    store, sent, settle,
    emit: (name, e) => { (listeners[name] || []).forEach((fn) => fn(e)); },
    seed: (parcels, key) => {
      store.parcels = JSON.stringify(parcels);
      if (key !== null) store.api_key = key || 'test-key';
    },
    lastOf: (type) => {
      for (let i = sent.length - 1; i >= 0; i--) if (sent[i].TYPE === type) return sent[i];
      return null;
    },
  };
}

const H = 3600000;
function results(milestone, events) {
  return { status: 200, body: { data: { trackings: [
    { tracker: { trackingNumber: 'X' }, shipment: { statusMilestone: milestone },
      events: events },
  ] } } };
}

// --- happy path: two parcels, packed for the list screen ------------------
{
  const app = boot((m, u) => {
    if (/AAA111/.test(u)) return results('in_transit', [
      { description: 'Processed at sorting center', location: 'Leipzig, DE',
        timestamp: new Date(Date.now() - 9 * H).toISOString() },
      { description: 'Departed facility', location: 'Bonn, DE',
        timestamp: new Date(Date.now() - 2 * H).toISOString() },
    ]);
    return results('delivered', [
      { description: 'Delivered', location: 'Singapore, SG',
        timestamp: new Date(Date.now() - 26 * H).toISOString() },
    ]);
  });
  app.seed([
    { nickname: 'Headphones', trackingNumber: 'AAA111' },
    { nickname: 'Camera lens', trackingNumber: 'BBB222' },
  ]);
  app.emit('appmessage', { payload: { REQUEST: 'list' } });
  app.settle();

  const first = app.sent[0];
  check('answers immediately, before the network', first.TYPE === 'parcels', first.TYPE);
  check('first answer flags more to come', first.INDEX === 1, first.INDEX);

  const final = app.lastOf('parcels');
  check('final answer clears the pending flag', final.INDEX === 0, final.INDEX);
  const rows = final.PAYLOAD.split('\n');
  check('one row per parcel', rows.length === 2, rows.length);

  const f = rows[0].split(US);
  check('row has four fields', f.length === 4, f.length);
  check('nickname first', f[0] === 'Headphones', f[0]);
  check('milestone code second (drives the row colour)', f[1] === 'in_transit', f[1]);
  check('human label third', f[2] === 'In transit', f[2]);
  check('age of the newest event last', f[3] === '2h ago', f[3]);
  check('delivered parcel labelled', rows[1].split(US)[2] === 'Delivered',
    rows[1].split(US)[2]);
  check('day-old event reported in days', rows[1].split(US)[3] === '1d ago',
    rows[1].split(US)[3]);

  // Events for parcel 0.
  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();
  const ev = app.lastOf('events').PAYLOAD.split('\n');
  check('newest event first', /Departed facility/.test(ev[0]), ev[0]);
  check('event row has three fields', ev[0].split(US).length === 3, ev[0].split(US).length);
  check('event carries its location', ev[0].split(US)[2] === 'Bonn, DE',
    ev[0].split(US)[2]);
}

// --- caching: a second look doesn't re-hit Ship24 -------------------------
{
  let calls = 0;
  const app = boot(() => { calls++; return results('in_transit', []); });
  app.seed([{ nickname: 'A', trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'list' } });
  app.settle();
  check('one call for one parcel', calls === 1, calls);

  app.emit('appmessage', { payload: { REQUEST: 'list' } });
  app.settle();
  check('cached list served without another call', calls === 1, calls);

  app.emit('appmessage', { payload: { REQUEST: 'refresh' } });
  app.settle();
  check('explicit refresh bypasses the cache', calls === 2, calls);
}

// --- failures land on the row, not on a blank screen ----------------------
{
  const app = boot(() => ({ status: 401 }));
  app.seed([{ nickname: 'Headphones', trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'list' } });
  app.settle();
  const row = app.lastOf('parcels').PAYLOAD.split(US);
  check('401 explained on the parcel row', row[2] === 'Check your API key', row[2]);
  check('failed row keeps its nickname', row[0] === 'Headphones', row[0]);
  check('error label fits the C buffer', utf8Len(row[2]) < C_BUFFERS.label,
    utf8Len(row[2]));
}
{
  const app = boot(() => ({ status: 200, body: {} }));
  app.seed([{ nickname: 'A', trackingNumber: 'AAA111' }], null);
  app.emit('appmessage', { payload: { REQUEST: 'list' } });
  app.settle();
  const err = app.sent[app.sent.length - 1];
  check('no API key gives a dedicated error screen', err.TYPE === 'error', err.TYPE);
  check('error names the fix', /API key/.test(err.ERROR), err.ERROR);
}

// --- byte budgets: nothing may overflow a C buffer ------------------------
{
  const longAscii = 'Shipment has departed from the international sorting facility ' +
    'and is on its way to the destination country where it will clear customs ' +
    'before being handed to the final delivery partner for the last mile';
  const longCJK = '包裹已离开国际分拣中心正在运往目的地国家'.repeat(6);
  const app = boot(() => results('in_transit', [
    { description: longAscii, location: 'A very long facility name, Somewhere, DE',
      timestamp: new Date(Date.now() - H).toISOString() },
    { description: longCJK, location: '新加坡樟宜机场物流中心分拣站点',
      timestamp: new Date(Date.now() - 2 * H).toISOString() },
  ]));
  app.seed([{ nickname: 'A parcel with a really quite long nickname indeed',
              trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'list' } });
  app.settle();
  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();

  const nick = app.lastOf('parcels').PAYLOAD.split(US)[0];
  check('long nickname fits ParcelRow.nickname',
    utf8Len(nick) < C_BUFFERS.nickname, utf8Len(nick));

  const rows = app.lastOf('events').PAYLOAD.split('\n');
  for (let i = 0; i < rows.length; i++) {
    const f = rows[i].split(US);
    check('event ' + i + ' when fits its buffer', utf8Len(f[0]) < C_BUFFERS.when,
      utf8Len(f[0]));
    check('event ' + i + ' description fits its buffer',
      utf8Len(f[1]) < C_BUFFERS.description, utf8Len(f[1]));
    check('event ' + i + ' location fits its buffer',
      utf8Len(f[2]) < C_BUFFERS.location, utf8Len(f[2]));
  }
  check('long ASCII description is elided, not silently dropped',
    /…$/.test(rows[0].split(US)[1]), rows[0].split(US)[1]);

  // A cut in the middle of a multibyte character would render as garbage.
  const cjk = rows[1].split(US)[1];
  check('CJK text cut on a character boundary',
    Buffer.from(cjk, 'utf8').toString('utf8') === cjk && !/�/.test(cjk), cjk);
  check('CJK location cut on a character boundary',
    !/�/.test(rows[1].split(US)[2]), rows[1].split(US)[2]);
}

// --- separators must never appear inside a field --------------------------
{
  const app = boot(() => results('in_transit', [
    { description: 'Line one\nline two\ttabbed\x1fseparated', location: 'X\nY',
      timestamp: new Date(Date.now() - H).toISOString() },
  ]));
  app.seed([{ nickname: 'A', trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();
  const line = app.lastOf('events').PAYLOAD;
  check('carrier text cannot inject extra records', line.split('\n').length === 1,
    JSON.stringify(line));
  check('carrier text cannot inject extra fields', line.split(US).length === 3,
    line.split(US).length);
}

// --- undated events sink to the bottom instead of jumping to the top ------
{
  const app = boot(() => results('in_transit', [
    { description: 'No date on this one', location: '' },
    { description: 'Dated', location: '', timestamp: new Date(Date.now() - H).toISOString() },
  ]));
  app.seed([{ nickname: 'A', trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();
  const rows = app.lastOf('events').PAYLOAD.split('\n');
  check('dated event ranks above the undated one', /Dated/.test(rows[0]), rows[0]);
  check('undated event still shown', /No date/.test(rows[1]), rows[1]);
}

// --- the live API's own event shape, not just the docs' simplified one ----
{
  const app = boot(() => results('in_transit', [
    { status: 'Arrived at destination', location: 'Paris, FR',
      occurrenceDatetime: '2024-01-15T10:30:00', utcOffset: '+01:00' },
  ]));
  app.seed([{ nickname: 'A', trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();
  const f = app.lastOf('events').PAYLOAD.split(US);
  check('falls back to status when there is no description',
    f[1] === 'Arrived at destination', f[1]);
  check('occurrenceDatetime + utcOffset parses to a real age',
    /ago$/.test(f[0]) || f[0] === 'just now', f[0]);
}

// --- settings round trip --------------------------------------------------
{
  const registered = [];
  const app = boot((m, u, body) => {
    if (m === 'POST') {
      registered.push(JSON.parse(body).trackingNumber);
      return { status: 200, body: { data: { tracker: { trackerId: 'tr_' + registered.length } } } };
    }
    return results('in_transit', []);
  });
  app.seed([{ nickname: 'Old', trackingNumber: 'OLD999', trackerId: 'tr_old' }]);
  app.store.cache_OLD999 = JSON.stringify({ fetchedAt: Date.now(), milestone: 'delivered', events: [] });

  app.emit('webviewclosed', { response: encodeURIComponent(JSON.stringify({
    action: 'save',
    apiKey: 'new-key',
    parcels: [{ nickname: 'New', trackingNumber: 'NEW111', courierCode: 'dhl' }],
  })) });
  app.settle();

  check('api key stored', app.store.api_key === 'new-key', app.store.api_key);
  check('new tracking number registered with Ship24',
    registered.length === 1 && registered[0] === 'NEW111', registered.join(','));
  check('trackerId kept for next time',
    JSON.parse(app.store.parcels)[0].trackerId === 'tr_1',
    JSON.parse(app.store.parcels)[0].trackerId);
  check('removed parcel\'s cache dropped', app.store.cache_OLD999 === undefined,
    app.store.cache_OLD999);
  check('watch gets the new list', /New/.test(app.lastOf('parcels').PAYLOAD),
    app.lastOf('parcels').PAYLOAD);

  // Saving again must not re-register an unchanged parcel.
  app.emit('webviewclosed', { response: encodeURIComponent(JSON.stringify({
    action: 'save',
    parcels: [{ nickname: 'New', trackingNumber: 'NEW111', courierCode: 'dhl' }],
  })) });
  app.settle();
  check('unchanged parcel not re-registered', registered.length === 1, registered.length);
  check('blank key on save keeps the stored one', app.store.api_key === 'new-key',
    app.store.api_key);
}

console.log(failures ? '\n' + failures + ' FAILED' : '\nall pkjs checks passed');
process.exit(failures ? 1 : 0);
