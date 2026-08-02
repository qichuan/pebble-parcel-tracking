// Phone-side companion. The watch can't make HTTP calls, so everything here:
// the Ship24 API key and parcel list (entered in the settings page), tracker
// registration, result caching, and packing replies into compact strings the
// C side renders verbatim.
//
// Nothing is sent to any server but Ship24 — the API key is the user's own and
// never leaves the phone except in the Authorization header.

var API_BASE = 'https://api.ship24.com/public/v1';
var CONFIG_URL = 'https://qichuan.github.io/pebble-parcel-tracking/';

// Field/record separators shared with the C side. US (unit separator, 0x1f)
// never appears in carrier text, so it's safe as a field delimiter.
var US = '\x1f';

var CACHE_TTL_MS = 5 * 60 * 1000; // don't re-hit Ship24 more often than this
var REQUEST_TIMEOUT_MS = 20000;

// Kept in step with the buffer sizes in src/c/*.c — see MAX_* there.
var MAX_PARCELS = 12;
var MAX_EVENTS = 12;
var MAX_NICKNAME_BYTES = 36;
var MAX_DESC_BYTES = 160; // the event detail screen scrolls, so give it room
var MAX_LOCATION_BYTES = 40;
var MAX_LABEL_BYTES = 30;

// --- storage -------------------------------------------------------------

function apiKey() {
  return localStorage.getItem('api_key') || '';
}

function loadParcels() {
  try {
    var raw = JSON.parse(localStorage.getItem('parcels'));
    return Object.prototype.toString.call(raw) === '[object Array]' ? raw : [];
  } catch (e) {
    return [];
  }
}

function saveParcels(parcels) {
  localStorage.setItem('parcels', JSON.stringify(parcels));
}

function cacheKey(trackingNumber) {
  return 'cache_' + trackingNumber;
}

function loadCache(trackingNumber) {
  try {
    return JSON.parse(localStorage.getItem(cacheKey(trackingNumber)));
  } catch (e) {
    return null;
  }
}

function saveCache(trackingNumber, value) {
  localStorage.setItem(cacheKey(trackingNumber), JSON.stringify(value));
}

function isStale(cached) {
  return !cached || (Date.now() - (cached.fetchedAt || 0)) > CACHE_TTL_MS;
}

// --- Ship24 --------------------------------------------------------------

// callback(err, data). `err` is already a short, watch-friendly message.
function ship24(method, path, body, callback) {
  var key = apiKey();
  if (!key) {
    return callback('Set API key in settings');
  }
  var xhr = new XMLHttpRequest();
  xhr.open(method, API_BASE + path);
  xhr.setRequestHeader('Authorization', 'Bearer ' + key);
  xhr.setRequestHeader('Accept', 'application/json');
  if (body) {
    xhr.setRequestHeader('Content-Type', 'application/json');
  }
  xhr.timeout = REQUEST_TIMEOUT_MS;
  xhr.onload = function () {
    var data = null;
    try {
      data = JSON.parse(xhr.responseText);
    } catch (e) {}
    if (xhr.status >= 200 && xhr.status < 300) {
      return callback(null, data);
    }
    console.log('Ship24 ' + method + ' ' + path + ' -> ' + xhr.status + ' ' +
                xhr.responseText);
    if (xhr.status === 401) return callback('Check your API key');
    if (xhr.status === 403) return callback('Not on your Ship24 plan');
    if (xhr.status === 404) return callback('Tracking number not found');
    if (xhr.status === 429) return callback('Ship24 rate limit');
    if (xhr.status >= 500) return callback('Ship24 unavailable');
    return callback('Ship24 error ' + xhr.status);
  };
  xhr.onerror = function () { callback('No connection'); };
  xhr.ontimeout = function () { callback('Ship24 timed out'); };
  xhr.send(body ? JSON.stringify(body) : null);
}

// Registers the number with Ship24 so it starts collecting events. Failures are
// non-fatal: an already-registered number still answers the search endpoint.
function registerTracker(parcel, callback) {
  var body = { trackingNumber: parcel.trackingNumber };
  if (parcel.courierCode) {
    body.courierCode = parcel.courierCode;
  }
  ship24('POST', '/trackers', body, function (err, data) {
    if (!err && data && data.data && data.data.tracker) {
      parcel.trackerId = data.data.tracker.trackerId;
    } else if (err) {
      console.log('register ' + parcel.trackingNumber + ': ' + err);
    }
    callback();
  });
}

// Ship24's own docs simplify the event shape; the live API uses
// occurrenceDatetime/datetime + utcOffset. Accept every spelling.
function eventTimeMs(ev) {
  var raw = ev.timestamp || ev.occurrenceDatetime || ev.datetime;
  if (!raw) return null;
  var s = String(raw);
  if (!/(Z|[+-]\d{2}:?\d{2})$/.test(s)) {
    s += ev.utcOffset || 'Z'; // no offset given: assume the carrier meant UTC
  }
  var t = Date.parse(s);
  return isNaN(t) ? null : t;
}

function eventDescription(ev) {
  return ev.description || ev.status || ev.statusCode || ev.statusMilestone || '';
}

// Why the last fetch of a given tracking number failed, by tracking number.
// Surfaced on the watch row when there's no cached result to show instead; not
// persisted, since every attempt recomputes it.
var s_errors = {};

// GET the current state of one parcel and cache it.
function refreshParcel(parcel, callback) {
  var path = '/trackers/search/' + encodeURIComponent(parcel.trackingNumber) +
             '/results';
  ship24('GET', path, null, function (err, data) {
    if (err) {
      s_errors[parcel.trackingNumber] = err;
      return callback(err);
    }
    delete s_errors[parcel.trackingNumber];
    var trackings = (data && data.data && data.data.trackings) || [];
    var t = trackings[0] || {};
    var milestone = (t.shipment && t.shipment.statusMilestone) ||
                    (t.tracker && t.tracker.statusMilestone) || '';

    var events = [];
    var raw = t.events || [];
    for (var i = 0; i < raw.length; i++) {
      events.push({
        at: eventTimeMs(raw[i]),
        description: truncateBytes(eventDescription(raw[i]), MAX_DESC_BYTES),
        location: truncateBytes(raw[i].location || '', MAX_LOCATION_BYTES),
      });
    }
    // Newest first. Events with no parsable date keep their original order at
    // the end rather than jumping to the top.
    events.sort(function (a, b) {
      if (a.at === null && b.at === null) return 0;
      if (a.at === null) return 1;
      if (b.at === null) return -1;
      return b.at - a.at;
    });
    events = events.slice(0, MAX_EVENTS);

    saveCache(parcel.trackingNumber, {
      fetchedAt: Date.now(),
      milestone: milestone,
      events: events,
    });
    callback(null);
  });
}

// Refreshes `list` one at a time — parallel requests are the quickest way to
// trip Ship24's rate limiter. Individual failures are recorded in s_errors and
// surfaced per row, so the caller only needs to know when the pass is done.
function refreshSequentially(list, callback) {
  var i = 0;
  function step() {
    if (i >= list.length) {
      return callback();
    }
    refreshParcel(list[i++], step);
  }
  step();
}

// --- formatting ----------------------------------------------------------

var MILESTONE_LABELS = {
  pending: 'Pending',
  info_received: 'Info received',
  in_transit: 'In transit',
  out_for_delivery: 'Out for delivery',
  failed_attempt: 'Failed attempt',
  available_for_pickup: 'For pickup',
  delivered: 'Delivered',
  exception: 'Exception',
};

function milestoneLabel(milestone) {
  if (MILESTONE_LABELS[milestone]) return MILESTONE_LABELS[milestone];
  return milestone ? milestone.replace(/_/g, ' ') : 'Unknown';
}

// The C side copies into fixed byte buffers, so cut on a character boundary
// with the byte budget in mind — a mid-character cut renders as garbage.
function truncateBytes(str, maxBytes) {
  if (!str) return '';
  var clean = String(str).replace(/[\r\n\t\x1f]+/g, ' ').replace(/ {2,}/g, ' ');
  clean = clean.replace(/^ +| +$/g, '');

  var bytes = 0;
  var i = 0;
  while (i < clean.length) {
    var code = clean.charCodeAt(i);
    var charLen = 1;
    var cost;
    if (code < 0x80) {
      cost = 1;
    } else if (code < 0x800) {
      cost = 2;
    } else if (code >= 0xd800 && code < 0xdc00 && i + 1 < clean.length) {
      cost = 4; // surrogate pair
      charLen = 2;
    } else {
      cost = 3;
    }
    if (bytes + cost > maxBytes) {
      return clean.substring(0, i) + '…';
    }
    bytes += cost;
    i += charLen;
  }
  return clean;
}

function timeAgo(ms) {
  if (!ms) return '';
  var diff = Date.now() - ms;
  if (diff < 0) return 'just now';
  var mins = Math.floor(diff / 60000);
  if (mins < 1) return 'just now';
  if (mins < 60) return mins + 'm ago';
  var hours = Math.floor(mins / 60);
  if (hours < 24) return hours + 'h ago';
  var days = Math.floor(hours / 24);
  if (days < 7) return days + 'd ago';
  var weeks = Math.floor(days / 7);
  if (weeks < 5) return weeks + 'w ago';
  return Math.floor(days / 30) + 'mo ago';
}

// --- packing -------------------------------------------------------------

// "nickname<US>milestone<US>label<US>ago" per parcel.
function packParcels() {
  var parcels = loadParcels().slice(0, MAX_PARCELS);
  var lines = [];
  for (var i = 0; i < parcels.length; i++) {
    var p = parcels[i];
    var cached = loadCache(p.trackingNumber);
    var milestone = cached ? cached.milestone : '';
    var latest = cached && cached.events && cached.events.length
        ? cached.events[0].at : null;
    // A cached result outranks a failed refresh — stale status beats no status.
    var label;
    if (cached) {
      label = milestoneLabel(milestone);
    } else if (s_errors[p.trackingNumber]) {
      label = truncateBytes(s_errors[p.trackingNumber], MAX_LABEL_BYTES);
    } else {
      label = 'Checking…';
    }
    lines.push([
      truncateBytes(p.nickname || p.trackingNumber, MAX_NICKNAME_BYTES),
      milestone,
      label,
      timeAgo(latest),
    ].join(US));
  }
  return lines.join('\n');
}

// "when<US>description<US>location" per event, newest first.
function packEvents(trackingNumber) {
  var cached = loadCache(trackingNumber);
  if (!cached || !cached.events) return '';
  var lines = [];
  for (var i = 0; i < cached.events.length; i++) {
    var e = cached.events[i];
    lines.push([timeAgo(e.at), e.description, e.location].join(US));
  }
  return lines.join('\n');
}

// --- watch messaging -----------------------------------------------------

function send(type, payload, pending) {
  var msg = { TYPE: type, PAYLOAD: payload, INDEX: pending ? 1 : 0 };
  Pebble.sendAppMessage(msg, null, function (e) {
    console.log('sendAppMessage failed: ' + JSON.stringify(e));
  });
}

function sendError(message) {
  Pebble.sendAppMessage({ TYPE: 'error', ERROR: message }, null, function (e) {
    console.log('sendAppMessage failed: ' + JSON.stringify(e));
  });
}

// Answers from cache immediately so the list draws at once, then refreshes what
// has gone stale and sends the list again.
function deliverParcels(force) {
  var parcels = loadParcels().slice(0, MAX_PARCELS);
  if (!parcels.length) {
    return send('parcels', '', false);
  }
  if (!apiKey()) {
    return sendError('Set API key in settings');
  }

  var stale = [];
  for (var i = 0; i < parcels.length; i++) {
    if (force || isStale(loadCache(parcels[i].trackingNumber))) {
      stale.push(parcels[i]);
    }
  }
  send('parcels', packParcels(), stale.length > 0);
  if (!stale.length) return;

  // Failures don't get their own error screen here: packParcels puts the reason
  // on the affected row, so one dead tracking number can't hide the others.
  refreshSequentially(stale, function () {
    send('parcels', packParcels(), false);
  });
}

function deliverEvents(index) {
  var parcels = loadParcels().slice(0, MAX_PARCELS);
  var parcel = parcels[index];
  if (!parcel) {
    return sendError('Parcel not found');
  }
  var cached = loadCache(parcel.trackingNumber);
  // Always answer at once — the watch re-asks if a request goes unanswered, and
  // a cold fetch can easily outlast that window. An empty payload with the
  // pending flag set keeps the watch on its loading screen.
  send('events', cached ? packEvents(parcel.trackingNumber) : '',
       isStale(cached));
  if (cached && !isStale(cached)) return;

  refreshParcel(parcel, function (err) {
    if (err) {
      if (!cached) sendError(err);
      return;
    }
    send('events', packEvents(parcel.trackingNumber), false);
  });
}

Pebble.addEventListener('ready', function () {
  console.log('Parcel Tracking pkjs ready');
  // The watch asks for the list as soon as it launches, which can be before
  // this JS is running — that request is dropped, so answer it here too.
  deliverParcels(false);
});

Pebble.addEventListener('appmessage', function (e) {
  var request = e.payload.REQUEST;
  if (request === 'list') {
    deliverParcels(false);
  } else if (request === 'refresh') {
    deliverParcels(true);
  } else if (request === 'detail') {
    deliverEvents(e.payload.INDEX || 0);
  }
});

// --- settings ------------------------------------------------------------

Pebble.addEventListener('showConfiguration', function () {
  // Only the parcel list round-trips to the page; the API key never does.
  var stored = loadParcels();
  var parcels = [];
  for (var i = 0; i < stored.length; i++) {
    parcels.push({
      nickname: stored[i].nickname || '',
      trackingNumber: stored[i].trackingNumber,
      courierCode: stored[i].courierCode || '',
    });
  }
  Pebble.openURL(CONFIG_URL + '?parcels=' +
                 encodeURIComponent(JSON.stringify(parcels)) +
                 '&has_key=' + (apiKey() ? '1' : '0'));
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e.response) return;
  var resp;
  try {
    resp = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    return;
  }
  if (resp.action !== 'save') return;

  if (resp.apiKey) {
    localStorage.setItem('api_key', String(resp.apiKey).replace(/^\s+|\s+$/g, ''));
  }

  var previous = loadParcels();
  var incoming = resp.parcels || [];
  var next = [];
  var kept = {};
  for (var i = 0; i < incoming.length && next.length < MAX_PARCELS; i++) {
    var number = String(incoming[i].trackingNumber || '').replace(/\s+/g, '');
    if (!number || kept[number]) continue;
    kept[number] = true;
    // Carry the trackerId over so an unchanged parcel isn't re-registered.
    var trackerId = '';
    for (var j = 0; j < previous.length; j++) {
      if (previous[j].trackingNumber === number) {
        trackerId = previous[j].trackerId || '';
        break;
      }
    }
    next.push({
      nickname: String(incoming[i].nickname || '').replace(/^\s+|\s+$/g, '') || number,
      trackingNumber: number,
      courierCode: String(incoming[i].courierCode || '').replace(/\s+/g, ''),
      trackerId: trackerId,
    });
  }

  // Drop cached results for parcels the user removed.
  for (var k = 0; k < previous.length; k++) {
    if (!kept[previous[k].trackingNumber]) {
      localStorage.removeItem(cacheKey(previous[k].trackingNumber));
    }
  }
  saveParcels(next);

  // Register anything new, then push a fresh list to the watch.
  var unregistered = [];
  for (var n = 0; n < next.length; n++) {
    if (!next[n].trackerId) unregistered.push(next[n]);
  }
  var index = 0;
  function registerNext() {
    if (index >= unregistered.length) {
      saveParcels(next); // persist the trackerIds we just collected
      return deliverParcels(true);
    }
    registerTracker(unregistered[index++], registerNext);
  }
  registerNext();
});
