// Drives docs/index.html in jsdom and checks the payload it hands back to pkjs.
const fs = require('fs');
const path = require('path');
const { JSDOM } = require('jsdom');

const HTML = fs.readFileSync(
  path.join(__dirname, '..', 'docs', 'index.html'), 'utf8');

let failures = 0;
function check(name, cond, extra) {
  console.log((cond ? 'PASS  ' : 'FAIL  ') + name + (cond ? '' : '  -> ' + extra));
  if (!cond) failures++;
}

function open(search) {
  // return_to=# makes the page's `location.href = returnTo + payload` a plain
  // fragment change, which jsdom performs (a pebblejs:// scheme would not) —
  // the concatenation and encoding under test are identical either way.
  const url = 'https://qichuan.github.io/pebble-parcel-tracking/' + search +
              '&return_to=%23';
  const dom = new JSDOM(HTML, { url: url, runScripts: 'dangerously' });
  const win = dom.window;
  return {
    win,
    doc: win.document,
    saved: () => (win.location.hash.length > 1 ? win.location.hash : null),
  };
}

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
  doc.querySelector('.parcel .courier').value = 'DHL';
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
  check('courier lowercased', payload.parcels[0].courierCode === 'dhl',
    payload.parcels[0].courierCode);
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

console.log(failures ? '\n' + failures + ' FAILED' : '\nall settings-page checks passed');
process.exit(failures ? 1 : 0);
