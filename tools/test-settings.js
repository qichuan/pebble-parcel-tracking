// Drives docs/index.html in jsdom and checks the payload it hands back to pkjs.
const fs = require('fs');
const path = require('path');
const { JSDOM } = require('jsdom');

const HTML = fs.readFileSync(
  path.join(__dirname, '..', 'docs', 'index.html'), 'utf8');
const COURIERS_JS = fs.readFileSync(
  path.join(__dirname, '..', 'docs', 'couriers.js'), 'utf8');

// jsdom won't fetch <script src>, so the generated courier list is inlined
// here. Dropping it instead is how the page behaves when couriers.js 404s.
const SCRIPT_TAG = '<script src="couriers.js" charset="utf-8"></script>';
function withCouriers(present) {
  if (HTML.indexOf(SCRIPT_TAG) === -1) throw new Error('courier script tag moved');
  return HTML.replace(SCRIPT_TAG,
    present ? '<script>' + COURIERS_JS + '</script>' : '');
}

let failures = 0;
function check(name, cond, extra) {
  console.log((cond ? 'PASS  ' : 'FAIL  ') + name + (cond ? '' : '  -> ' + extra));
  if (!cond) failures++;
}

function open(search, withList) {
  // return_to=# makes the page's `location.href = returnTo + payload` a plain
  // fragment change, which jsdom performs (a pebblejs:// scheme would not) —
  // the concatenation and encoding under test are identical either way.
  const url = 'https://qichuan.github.io/pebble-parcel-tracking/' + search +
              '&return_to=%23';
  const dom = new JSDOM(withCouriers(withList !== false),
    { url: url, runScripts: 'dangerously' });
  const win = dom.window;
  const type = (el, text) => {
    el.value = text;
    el.dispatchEvent(new win.Event('input', { bubbles: true }));
  };
  return {
    win,
    doc: win.document,
    type,
    // What the user sees in a card's courier box, and the code it would save.
    courier: (card) => card.querySelector('.cx-input').value,
    saved: () => (win.location.hash.length > 1 ? win.location.hash : null),
  };
}

function payloadOf(saved) { return JSON.parse(decodeURIComponent(saved.substring(1))); }

// --- fresh install: no key, no parcels ---
{
  const { doc, saved } = open('?parcels=%5B%5D&has_key=0');
  check('one empty parcel card on first run',
    doc.querySelectorAll('.parcel').length === 1, doc.querySelectorAll('.parcel').length);
  check('key field prompts for entry',
    doc.getElementById('api_key').placeholder === 'Paste your API key',
    doc.getElementById('api_key').placeholder);

  doc.getElementById('save').click();
  check('refuses to save without an API key', saved() === null, saved());
  check('and says why', /API key/.test(doc.getElementById('msg').textContent),
    doc.getElementById('msg').textContent);

  doc.getElementById('api_key').value = 'sk-test-123';
  doc.querySelector('.parcel .number').value = ' aaa 111 ';
  doc.querySelector('.parcel .nickname').value = '  Headphones ';
  doc.querySelector('.parcel .cx-input').value = 'DHL';
  doc.getElementById('save').click();

  const payload = JSON.parse(decodeURIComponent(saved().substring(1)));
  check('defaults to the pebblejs close URL when no return_to is given',
    HTML.indexOf("'pebblejs://close#'") !== -1, 'default missing');
  check('action is save', payload.action === 'save', payload.action);
  check('api key passed through', payload.apiKey === 'sk-test-123', payload.apiKey);
  check('tracking number stripped of spaces',
    payload.parcels[0].trackingNumber === 'aaa111', payload.parcels[0].trackingNumber);
  check('nickname trimmed', payload.parcels[0].nickname === 'Headphones',
    JSON.stringify(payload.parcels[0].nickname));
  check('a courier typed by name resolves to its code',
    payload.parcels[0].courierCode === 'dhl', payload.parcels[0].courierCode);
}

// --- returning user: existing parcels prefilled, key already stored ---
{
  const existing = [
    { nickname: 'Camera lens', trackingNumber: 'BBB222', courierCode: '' },
    { nickname: 'Coffee', trackingNumber: 'CCC333', courierCode: 'ups' },
  ];
  const { doc, saved } = open('?parcels=' + encodeURIComponent(JSON.stringify(existing)) +
                              '&has_key=1');
  check('existing parcels prefilled', doc.querySelectorAll('.parcel').length === 2,
    doc.querySelectorAll('.parcel').length);
  check('second number prefilled',
    doc.querySelectorAll('.parcel .number')[1].value === 'CCC333',
    doc.querySelectorAll('.parcel .number')[1].value);
  check('stored key shown as unchanged',
    /unchanged/.test(doc.getElementById('api_key').placeholder),
    doc.getElementById('api_key').placeholder);
  check('cards are numbered',
    doc.querySelectorAll('.parcel b')[1].textContent === 'PARCEL 2',
    doc.querySelectorAll('.parcel b')[1].textContent);

  // Remove the first, add a new one.
  doc.querySelectorAll('.parcel .remove')[0].click();
  check('remove drops the card', doc.querySelectorAll('.parcel').length === 1,
    doc.querySelectorAll('.parcel').length);
  check('remaining card renumbered to 1',
    doc.querySelector('.parcel b').textContent === 'PARCEL 1',
    doc.querySelector('.parcel b').textContent);

  doc.getElementById('add').click();
  doc.querySelectorAll('.parcel .number')[1].value = 'DDD444';
  doc.getElementById('save').click();
  const payload = JSON.parse(decodeURIComponent(saved().substring(1)));
  check('saves without re-entering the key', payload.apiKey === undefined,
    payload.apiKey);
  check('two parcels saved', payload.parcels.length === 2, payload.parcels.length);
  check('nickname defaults to the tracking number',
    payload.parcels[1].nickname === 'DDD444', payload.parcels[1].nickname);
}

// --- duplicates and blank rows ---
{
  const { doc, saved } = open('?parcels=%5B%5D&has_key=1');
  doc.querySelector('.parcel .number').value = 'XYZ';
  doc.getElementById('add').click();
  doc.querySelectorAll('.parcel .number')[1].value = 'XYZ';
  doc.getElementById('save').click();
  check('duplicate tracking numbers rejected', saved() === null, saved());
  check('duplicate warning names the number',
    /XYZ/.test(doc.getElementById('msg').textContent),
    doc.getElementById('msg').textContent);

  doc.querySelectorAll('.parcel .number')[1].value = '';
  doc.getElementById('save').click();
  const payload = JSON.parse(decodeURIComponent(saved().substring(1)));
  check('blank card ignored rather than saved', payload.parcels.length === 1,
    payload.parcels.length);
}

// --- the 12-parcel ceiling matches MAX_PARCELS in pkjs ---
{
  const { doc } = open('?parcels=%5B%5D&has_key=1');
  for (let i = 0; i < 20; i++) doc.getElementById('add').click();
  check('caps at 12 parcel cards', doc.querySelectorAll('.parcel').length === 12,
    doc.querySelectorAll('.parcel').length);
}

// --- the courier picker ---
{
  const { doc, type, courier, saved } = open('?parcels=%5B%5D&has_key=1');
  const card = doc.querySelector('.parcel');
  const input = card.querySelector('.cx-input');
  const menu = card.querySelector('.cx-list');

  type(input, 'australia p');
  const rows = menu.querySelectorAll('li[data-i]');
  check('typing filters the courier list', rows.length > 0 && rows.length <= 40,
    rows.length);
  check('the best match leads', rows[0].textContent.indexOf('Australia Post') === 0,
    rows[0].textContent);
  check('each row shows the code it will save',
    rows[0].querySelector('.code').textContent === 'au-post',
    rows[0].querySelector('.code').textContent);

  // mousedown, because that is what the page listens for (it beats blur).
  rows[0].dispatchEvent(new doc.defaultView.MouseEvent('mousedown', { bubbles: true }));
  check('picking a row shows the courier name', courier(card) === 'Australia Post',
    courier(card));
  check('and closes the list', menu.style.display === 'none', menu.style.display);

  card.querySelector('.number').value = 'AAA111';
  doc.getElementById('save').click();
  check('the picked courier saves as its Ship24 code',
    payloadOf(saved()).parcels[0].courierCode === 'au-post',
    payloadOf(saved()).parcels[0].courierCode);
}

// --- matching by accent-free typing, by code, and keyboard selection ---
{
  const { doc, type } = open('?parcels=%5B%5D&has_key=1');
  const card = doc.querySelector('.parcel');
  const input = card.querySelector('.cx-input');
  const menu = card.querySelector('.cx-list');

  type(input, 'ceska');
  check('accents are matched without typing them',
    /Česká pošta/.test(menu.textContent), menu.textContent.slice(0, 60));

  type(input, 'dhl');
  check('an exact code match leads',
    menu.querySelector('li[data-i] .code').textContent === 'dhl',
    menu.querySelector('li[data-i] .code').textContent);

  const down = new doc.defaultView.KeyboardEvent('keydown', { keyCode: 40, bubbles: true });
  const enter = new doc.defaultView.KeyboardEvent('keydown', { keyCode: 13, bubbles: true });
  input.dispatchEvent(down);
  input.dispatchEvent(enter);
  check('arrow-down then enter picks the highlighted courier',
    card.querySelector('.courier').value === 'dhl',
    card.querySelector('.courier').value);
}

// --- what the picker refuses, and what it lets through ---
{
  const { doc, type, saved } = open('?parcels=%5B%5D&has_key=1');
  const card = doc.querySelector('.parcel');
  card.querySelector('.number').value = 'AAA111';

  type(card.querySelector('.cx-input'), 'not a courier!');
  doc.getElementById('save').click();
  check('text that is neither a courier nor a code blocks the save',
    saved() === null, saved());
  check('and the message names the parcel',
    /Parcel 1/.test(doc.getElementById('msg').textContent),
    doc.getElementById('msg').textContent);

  // An unlisted code — newly added by Ship24, or one of the deprecated ones —
  // still has to work, because the list is a snapshot.
  type(card.querySelector('.cx-input'), 'ZZ-Courier');
  doc.getElementById('save').click();
  check('an unlisted but code-shaped courier saves lowercased',
    payloadOf(saved()).parcels[0].courierCode === 'zz-courier',
    payloadOf(saved()).parcels[0].courierCode);

  type(card.querySelector('.cx-input'), '');
  doc.getElementById('save').click();
  check('a blank courier still means "let Ship24 detect it"',
    payloadOf(saved()).parcels[0].courierCode === '',
    JSON.stringify(payloadOf(saved()).parcels[0].courierCode));
}

// --- stored codes round-trip through the page unchanged ---
{
  const existing = [
    { nickname: 'Lens', trackingNumber: 'BBB222', courierCode: 'au-post' },
    { nickname: 'Beans', trackingNumber: 'CCC333', courierCode: 'legacy-code' },
  ];
  const { doc, courier, saved } = open(
    '?parcels=' + encodeURIComponent(JSON.stringify(existing)) + '&has_key=1');
  const cards = doc.querySelectorAll('.parcel');
  check('a known stored code shows as the courier name',
    courier(cards[0]) === 'Australia Post', courier(cards[0]));
  check('an unknown stored code shows as itself',
    courier(cards[1]) === 'legacy-code', courier(cards[1]));

  doc.getElementById('save').click();
  const parcels = payloadOf(saved()).parcels;
  check('reopening and saving keeps both codes',
    parcels[0].courierCode === 'au-post' && parcels[1].courierCode === 'legacy-code',
    JSON.stringify(parcels.map((p) => p.courierCode)));
}

// --- the generated list itself ---
{
  const win = {};
  new Function('window', COURIERS_JS)(win);
  const lines = String(win.COURIERS).split('\n');
  const codes = {};
  lines.forEach((l) => { const f = l.split('|'); codes[f[1]] = f[0]; });
  check('couriers.js carries the whole active list', lines.length > 1500, lines.length);
  check('a well-known courier is present', codes['au-post'] === 'Australia Post',
    codes['au-post']);
  // deprecated in the Ship24 export, so the picker must not offer it
  check('deprecated couriers are left out', codes['alphafast'] === undefined,
    codes['alphafast']);
  check('every row has a name and a code',
    lines.every((l) => { const f = l.split('|'); return f[0] && f[1]; }), 'malformed row');
}

// --- without couriers.js the field is the plain code box it used to be ---
{
  const { doc, saved } = open('?parcels=%5B%5D&has_key=1', false);
  const card = doc.querySelector('.parcel');
  check('the courier field still accepts a typed code',
    card.querySelector('.cx-input').placeholder === 'dhl',
    card.querySelector('.cx-input').placeholder);
  card.querySelector('.number').value = 'AAA111';
  card.querySelector('.cx-input').value = 'UPS';
  doc.getElementById('save').click();
  check('and saves it',
    payloadOf(saved()).parcels[0].courierCode === 'ups',
    payloadOf(saved()).parcels[0].courierCode);
}

console.log(failures ? '\n' + failures + ' FAILED' : '\nall settings-page checks passed');
process.exit(failures ? 1 : 0);
