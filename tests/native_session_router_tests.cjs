/** @summary Verify semantic guardrails of the embedded router without game processes or desktop input. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');

/** @summary Provide only native Client bindings, making DOM clicks and window-focus dependencies unavailable. */
function fixture(overrides = {}) {
    const calls = [];
    let claimed = false;
    const region = () => ({ id: 9, sandbox: true, boxes: [{ id: 21, uid: 1026, isClaimed: claimed }] });
    const defaults = {
        'isLoadingFinalized': false,
        'RegionTemplates': { regionTemplates: [{ key: 17, status: 'ACTIVE' }] },
        'GetRegionDetails/9': region,
        'CreateRegion/Test Region/17/true': { id: 9 },
        'CreateBox/9/21/Test City': () => { claimed = true; return { id: 21 }; },
        'activeBoxId': 21, 'ActiveCityUID': 1026, 'RegionID': 9,
    };
    const client = {
        RequestGameData(query) {
            calls.push(query);
            let value = Object.hasOwn(overrides, query) ? overrides[query] : defaults[query];
            if (typeof value === 'function') value = value();
            return value && typeof value === 'object' ? { json: value } : value;
        },
        PostGameCommand(command) { calls.push('command:' + command); },
        RequestUpdaterData(command) { calls.push('updater:' + command); return 2; },
    };
    const game = { cLauncher3D: { mPreloadDone: true } };
    const router = vm.runInNewContext(source, { window: { Client: client, simcity: game } });
    return { calls, game, claim() { claimed = true; }, run(packet) { return JSON.parse(router(JSON.stringify(packet))); } };
}

let test = fixture();
assert.equal(test.run({ op: 'observe' }).result.phase, 'main_menu');
assert.deepEqual(test.calls, ['isLoadingFinalized']);
test = fixture();
test.game.cLauncher3D.mPreloadDone = false;
assert.equal(test.run({ op: 'observe' }).result.phase, 'loading');
test.game.gCurtainUIHandler = { mMoviePlaying: true, StopIntroMovie() { this.mMoviePlaying = false; test.calls.push('native-intro-stop'); } };
assert.equal(test.run({ op: 'observe' }).result.phase, 'intro');
assert.equal(test.run({ op: 'skip-intro' }).result.wasPlaying, true);
assert.equal(test.game.gCurtainUIHandler.mMoviePlaying, false);
assert.equal(test.game.cLauncher3D.mSkipIntroMovie, true);
assert.equal(test.run({ op: 'observe' }).result.phase, 'loading');
assert.equal(test.run({ op: 'skip-intro' }).result.wasPlaying, false);
assert.equal(test.calls.filter(value => value === 'native-intro-stop').length, 1);

test = fixture();
test.game.gUpdater = { mCurrentState: 2 };
assert.equal(test.run({ op: 'enter-menu' }).result.queued, true);
assert.deepEqual(test.calls, ['updater:252943297', 'updater:2052759671']);

test = fixture();
test.game.gUpdater = { mCurrentState: 5 };
assert.equal(test.run({ op: 'enter-menu' }).ok, false);
assert(!test.calls.includes('updater:2052759671'));

test = fixture();
assert.equal(test.run({ op: 'create-sandbox-region', regionName: 'Test Region', templateId: 17 }).result.sandbox, true);
assert(test.calls.includes('CreateRegion/Test Region/17/true'));

test = fixture();
assert.equal(test.run({ op: 'create-sandbox-region', regionName: 'Bad/true', templateId: 17 }).ok, false);
assert(!test.calls.some(call => call.startsWith('CreateRegion/')));

test = fixture();
assert.equal(test.run({ op: 'create-sandbox-region', regionName: 'Test Region', templateId: 999 }).ok, false);
assert(!test.calls.some(call => call.startsWith('CreateRegion/')));

test = fixture();
assert.equal(test.run({ op: 'claim-city', regionId: 9, cityId: 21, cityName: 'Test City' }).result.city.isClaimed, true);
assert.equal(test.calls.filter(call => call.startsWith('CreateBox/')).length, 1);
assert.equal(test.run({ op: 'claim-city', regionId: 9, cityId: 21, cityName: 'Test City' }).ok, false);
assert.equal(test.calls.filter(call => call.startsWith('CreateBox/')).length, 1);

test = fixture({ 'GetRegionDetails/9': { id: 9, sandbox: false, boxes: [{ id: 21 }] } });
assert.equal(test.run({ op: 'claim-city', regionId: 9, cityId: 21, cityName: 'Test City' }).ok, false);
assert(!test.calls.some(call => call.startsWith('CreateBox/')));

test = fixture();
test.claim();
assert.deepEqual(test.run({ op: 'load-city', regionId: 9, cityId: 21 }).result,
    { queued: true, regionId: '9', cityId: '21', cityUid: '1026' });
assert.equal(test.calls.filter(call => call === 'command:loadGame/21').length, 1);

test = fixture({ 'isLoadingFinalized': true });
assert.deepEqual(test.run({ op: 'observe' }).result,
    { phase: 'city', loadingFinalized: true, cityId: '21', cityUid: '1026', regionId: '9' });

test = fixture({ 'GetRegionDetails/9': { errorCode: 11 } });
const partial = test.run({ op: 'create-sandbox-region', regionName: 'Test Region', templateId: 17 });
assert.equal(partial.ok, false);
assert.deepEqual(partial.partial, { createdRegionId: '9' });
test = fixture({ 'isLoadingFinalized': true, units: { '0,1': null, '1,2': null } });
assert.deepEqual(test.run({ op: 'world-units', regionId: 9, cityId: 21, argument: '1,1' }).result.unitRefs, ['1,2']);
assert.equal(test.run({ op: 'world-units', regionId: 9, cityId: 22, argument: '0,1' }).ok, false);
assert.equal(test.run({ op: 'world-unit', regionId: 9, cityId: 21, argument: '0,1/position' }).ok, false);
test = fixture({ 'isLoadingFinalized': true, 'GetRegionDetails/9': { sandbox: false } });
assert.equal(test.run({ op: 'world-units', regionId: 9, cityId: 21, argument: '0,1' }).ok, false);
console.log('Native session router: semantic transitions, injection/ownership guards, exact identities, Sandbox unit pages and partial outcomes passed.');
