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
  milestone: 24,  // main.c ParcelRow.milestone
  label: 36,      // main.c ParcelRow.label
  updated: 24,    // main.c s_updated
  day: 16,        // timeline_window.c EventRow.day
  description: 164, // timeline_window.c EventRow.label
  time: 60,       // timeline_window.c EventRow.time
  courier: 28,    // timeline_window.c s_courier
  when: 28,       // timeline_window.c s_when
  place: 32,      // timeline_window.c s_place
  eta: 28,        // timeline_window.c s_eta
  tracking: 36,   // timeline_window.c s_tracking
};

// Both payloads are a metadata record followed by rows, all US-delimited.
function unpack(msg) {
  const lines = msg.PAYLOAD.split('\n');
  return {
    meta: lines[0].split(US),
    rows: lines.slice(1).map((line) => line.split(US)),
  };
}

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
  const { meta, rows } = unpack(final);
  check('one row per parcel', rows.length === 2, rows.length);
  check('meta leads with the arriving-today count', meta[0] === '0', meta[0]);
  check('meta carries a clock time for the footer',
    /^\d{1,2}:\d{2} (AM|PM)$/.test(meta[1]), meta[1]);

  const f = rows[0];
  check('row has four fields', f.length === 4, f.length);
  check('nickname first', f[0] === 'Headphones', f[0]);
  check('milestone code second (drives the status colour)', f[1] === 'in_transit', f[1]);
  check('human label third', f[2] === 'In transit', f[2]);
  check('in-transit parcel not marked as arriving today', f[3] === '0', f[3]);
  check('delivered parcel labelled with when it landed',
    /^Delivered /.test(rows[1][2]), rows[1][2]);
  check('yesterday\'s delivery is not today\'s news', rows[1][3] === '0', rows[1][3]);

  // Events for parcel 0.
  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();
  const ev = unpack(app.lastOf('events'));
  check('detail meta names the courier slot', ev.meta.length === 7, ev.meta.length);
  check('detail meta carries the status', ev.meta[1] === 'In transit', ev.meta[1]);
  check('NOW AT is the newest event\'s city', ev.meta[3] === 'Bonn, DE', ev.meta[3]);
  check('EXPECTED falls back to no estimate', ev.meta[4] === 'No estimate', ev.meta[4]);
  check('detail meta carries the tracking number', ev.meta[5] === 'AAA111', ev.meta[5]);

  check('newest event first', /Departed facility/.test(ev.rows[0][1]), ev.rows[0][1]);
  check('event row has three fields', ev.rows[0].length === 3, ev.rows[0].length);
  check('first event opens a day group', ev.rows[0][0] !== '', ev.rows[0][0]);
  check('events in the same day group share one heading',
    ev.rows[1][0] === '' || ev.rows[1][0] !== ev.rows[0][0], ev.rows[1][0]);
  check('the current city is not repeated on its own row',
    !/Bonn/.test(ev.rows[0][2]), ev.rows[0][2]);
  check('a move between cities is marked where it happened',
    /Leipzig, DE/.test(ev.rows[1][2]), ev.rows[1][2]);
}

// --- the question the list screen exists to answer ------------------------
{
  const app = boot(() => results('out_for_delivery', [
    { description: 'OUT FOR DELIVERY', location: 'Tampines, SG',
      timestamp: new Date(Date.now() - 2 * H).toISOString() },
  ]));
  app.seed([{ nickname: 'Running shoes', trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'list' } });
  app.settle();
  const { meta, rows } = unpack(app.lastOf('parcels'));
  check('out for delivery counts as arriving today', meta[0] === '1', meta[0]);
  check('and is flagged for the row stripe', rows[0][3] === '1', rows[0][3]);

  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();
  const ev = unpack(app.lastOf('events'));
  check('EXPECTED says today without needing an estimate',
    ev.meta[4] === 'Arriving today', ev.meta[4]);
  check('shouted carrier text is calmed down',
    ev.rows[0][1] === 'Out for delivery', ev.rows[0][1]);
}

// --- a bare ETA date is a local calendar date, not UTC midnight -----------
// Date.parse("2026-08-06") is UTC midnight, which is still Aug 5 anywhere west
// of Greenwich — a parcel would be called due a day early.
{
  function ymd(offset) {
    const d = new Date();
    d.setDate(d.getDate() + offset);
    return d.getFullYear() + '-' + String(d.getMonth() + 1).padStart(2, '0') +
           '-' + String(d.getDate()).padStart(2, '0');
  }
  const app = boot(() => ({ status: 200, body: { data: { trackings: [{
    tracker: { trackingNumber: 'X' },
    shipment: { statusMilestone: 'in_transit',
                delivery: { estimatedDeliveryDate: ymd(0) } },
    events: [{ description: 'In transit', location: 'X',
               timestamp: new Date(Date.now() - H).toISOString() }],
  }] } } }));
  app.seed([{ nickname: 'A', trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'list' } });
  app.settle();
  const { meta, rows } = unpack(app.lastOf('parcels'));
  check('today\'s ETA counts as arriving today in local time', meta[0] === '1',
    meta[0]);
  check('and flags the row', rows[0][3] === '1', rows[0][3]);

  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();
  check('EXPECTED reads today, not yesterday',
    unpack(app.lastOf('events')).meta[4] === 'Arriving today',
    unpack(app.lastOf('events')).meta[4]);
}

// --- carrier boilerplate is rewritten, real detail is not -----------------
{
  const app = boot(() => results('in_transit', [
    { description: 'Handed over to Last Mile Carrier', location: 'X',
      timestamp: new Date(Date.now() - 1 * H).toISOString() },
    { description: 'Delivered to neighbour at 14B', location: 'X',
      timestamp: new Date(Date.now() - 2 * H).toISOString() },
  ]));
  app.seed([{ nickname: 'A', trackingNumber: 'AAA111' }]);
  app.emit('appmessage', { payload: { REQUEST: 'detail', INDEX: 0 } });
  app.settle();
  const { rows } = unpack(app.lastOf('events'));
  check('boilerplate phrase rewritten', rows[0][1] === 'With local courier',
    rows[0][1]);
  check('a phrase that only starts the same is left alone',
    rows[1][1] === 'Delivered to neighbour at 14B', rows[1][1]);
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
  const row = unpack(app.lastOf('parcels')).rows[0];
  check('401 explained on the parcel row', row[2] === 'Check your API key', row[2]);
  check('failed row keeps its nickname', row[0] === 'Headphones', row[0]);
  check('a parcel we know nothing about claims nothing about today',
    row[3] === '0', row[3]);
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

  const list = unpack(app.lastOf('parcels'));
  check('long nickname fits ParcelRow.nickname',
    utf8Len(list.rows[0][0]) < C_BUFFERS.nickname, utf8Len(list.rows[0][0]));
  check('milestone code fits ParcelRow.milestone',
    utf8Len(list.rows[0][1]) < C_BUFFERS.milestone, utf8Len(list.rows[0][1]));
  check('status label fits ParcelRow.label',
    utf8Len(list.rows[0][2]) < C_BUFFERS.label, utf8Len(list.rows[0][2]));
  check('update time fits s_updated',
    utf8Len(list.meta[1]) < C_BUFFERS.updated, utf8Len(list.meta[1]));

  const detail = unpack(app.lastOf('events'));
  const META_FIELDS = ['courier', 'label', 'when', 'place', 'eta', 'tracking',
                       'milestone'];
  for (let i = 0; i < META_FIELDS.length; i++) {
    const budget = C_BUFFERS[META_FIELDS[i]];
    if (!budget) continue;
    check('detail meta ' + META_FIELDS[i] + ' fits its buffer',
      utf8Len(detail.meta[i]) < budget, utf8Len(detail.meta[i]));
  }
  for (let i = 0; i < detail.rows.length; i++) {
    const f = detail.rows[i];
    check('event ' + i + ' day fits its buffer', utf8Len(f[0]) < C_BUFFERS.day,
      utf8Len(f[0]));
    check('event ' + i + ' description fits its buffer',
      utf8Len(f[1]) < C_BUFFERS.description, utf8Len(f[1]));
    check('event ' + i + ' time fits its buffer', utf8Len(f[2]) < C_BUFFERS.time,
      utf8Len(f[2]));
  }
  check('long ASCII description is elided, not silently dropped',
    /…$/.test(detail.rows[0][1]), detail.rows[0][1]);

  // A cut in the middle of a multibyte character would render as garbage.
  const cjk = detail.rows[1][1];
  check('CJK text cut on a character boundary',
    Buffer.from(cjk, 'utf8').toString('utf8') === cjk && !/�/.test(cjk), cjk);
  check('CJK location cut on a character boundary',
    !/�/.test(detail.rows[1][2]), detail.rows[1][2]);
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
  const { rows } = unpack(app.lastOf('events'));
  check('carrier text cannot inject extra records', rows.length === 1,
    JSON.stringify(app.lastOf('events').PAYLOAD));
  check('carrier text cannot inject extra fields', rows[0].length === 3,
    rows[0].length);
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
  const { rows } = unpack(app.lastOf('events'));
  check('dated event ranks above the undated one', /Dated/.test(rows[0][1]),
    rows[0][1]);
  check('undated event still shown', /No date/.test(rows[1][1]), rows[1][1]);
  check('undated event grouped under its own heading', rows[1][0] === 'UNDATED',
    rows[1][0]);
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
  const f = unpack(app.lastOf('events')).rows[0];
  check('falls back to status when there is no description',
    f[1] === 'Arrived at destination', f[1]);
  check('occurrenceDatetime + utcOffset parses to a real clock time',
    /^\d{1,2}:\d{2} (AM|PM)/.test(f[2]), f[2]);
}

// --- settings round trip --------------------------------------------------
{
  const registered = [];
  const app = boot((m, u, body) => {
    if (m === 'POST') {
      registered.push(JSON.parse(body).courierCode || JSON.parse(body).trackingNumber);
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
  check('new tracking number registered with Ship24, courier and all',
    registered.length === 1 && registered[0] === 'dhl', registered.join(','));
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

  // Picking a different courier has to reach Ship24, and the only thing that
  // carries it there is a registration — so the old trackerId is dropped.
  app.emit('webviewclosed', { response: encodeURIComponent(JSON.stringify({
    action: 'save',
    parcels: [{ nickname: 'New', trackingNumber: 'NEW111', courierCode: 'au-post' }],
  })) });
  app.settle();
  check('a changed courier re-registers the parcel',
    registered.length === 2 && registered[1] === 'au-post', registered.join(','));
  check('and the new courier is stored',
    JSON.parse(app.store.parcels)[0].courierCode === 'au-post',
    JSON.parse(app.store.parcels)[0].courierCode);
}

console.log(failures ? '\n' + failures + ' FAILED' : '\nall pkjs checks passed');
process.exit(failures ? 1 : 0);
