/** @summary Return one bounded, typed reply from the game's own Client bindings.
 * @param packetText JSON encoded by the owner-scoped native transport.
 * @return JSON text; native callers copy its UTF-16 result before releasing the scope.
 */
(function (packetText) {
    "use strict";
    var partial;
    try {
        var packet = JSON.parse(packetText);
        var client = window.Client;
        var game = window.simcity;
        if (!client || !game) throw new Error("The game UI Client is not ready.");

        /** @summary Preserve native JSON errors and unwrap the response envelope used by the game UI. */
        var data = function (query) {
            if (typeof client.RequestGameData !== "function") throw new Error("Game data bindings are not ready.");
            var value = client.RequestGameData(query);
            if (typeof value === "string") value = JSON.parse(value);
            if (value && Object.prototype.toString.call(value) === "[object Array]" && value.length === 1 && value[0] && value[0].json !== undefined) value = value[0];
            if (value && value.json !== undefined) value = value.json;
            if (value === undefined || value === null) throw new Error("The native query returned no result: " + query);
            if (value.error || (value.errorCode !== undefined && Number(value.errorCode) !== 0)) throw new Error("Native query failed: " + query + " (" + String(value.errorCode || value.error) + ")");
            return value;
        };

        /** @summary Validate a decimal native identifier without allowing query path injection. */
        var id = function (value) {
            var text = String(value);
            if (!/^[1-9][0-9]{0,9}$/.test(text) || Number(text) > 4294967295) throw new Error("Invalid native identifier.");
            return text;
        };

        /** @summary Validate a game-authored query segment before any region or city mutation. */
        var name = function (value) {
            if (typeof value !== "string" || !value.length || value.length > 128 || /[\/\x00-\x1f<>]/.test(value) || value.replace(/^\s+|\s+$/g, "") !== value) throw new Error("Invalid region or city name.");
            return value;
        };

        /** @summary Read current readiness and authoritative loaded-city identities independently. */
        var observe = function () {
            if (game.gUpdater && typeof client.RequestUpdaterData === "function") {
                return { phase: "updater", updaterState: game.gUpdater.mCurrentState, nativeUpdaterState: client.RequestUpdaterData(252943297), blocked: !!(game.gUpdater.mBlockPlay || game.gUpdater.mBlockUpdate || game.gUpdater.mBetaBlocked || game.gUpdater.mWaitForConfiguration) };
            }
            if (game.gCurtainUIHandler && game.gCurtainUIHandler.mMoviePlaying === true)
                return { phase: "intro", loadingFinalized: false, introPlaying: true };
            var finalized = data("isLoadingFinalized");
            if (!finalized) return { phase: game.cLauncher3D && game.cLauncher3D.mPreloadDone === true ? "main_menu" : "loading", loadingFinalized: false };
            var cityId = String(data("activeBoxId"));
            return { phase: cityId !== "0" ? "city" : "main_menu", loadingFinalized: !!finalized, cityId: cityId, cityUid: String(data("ActiveCityUID")), regionId: String(data("RegionID")) };
        };

        /** @summary Require an existing Sandbox region and select the requested box by its canonical ID. */
        var box = function (regionId, cityId) {
            var region = data("GetRegionDetails/" + id(regionId));
            if (region.sandbox !== true && region.sandbox !== "true") throw new Error("The target region is not Sandbox.");
            if (String(region.id) !== id(regionId) || !region.boxes) throw new Error("Region identity is inconsistent.");
            for (var index = 0; index < region.boxes.length; index++) {
                if (String(region.boxes[index].id) === id(cityId)) return { region: region, city: region.boxes[index] };
            }
            throw new Error("The requested city does not belong to the Sandbox region.");
        };

        var result;
        switch (packet.op) {
        case "skip-intro":
            if (game.cLauncher3D) game.cLauncher3D.mSkipIntroMovie = true;
            var curtain = game.gCurtainUIHandler;
            var wasPlaying = !!(curtain && curtain.mMoviePlaying === true);
            if (wasPlaying) {
                if (typeof curtain.StopIntroMovie !== "function") throw new Error("The native intro completion callback is not available.");
                curtain.StopIntroMovie();
            }
            result = { requested: true, wasPlaying: wasPlaying, introPlaying: !!(curtain && curtain.mMoviePlaying === true) };
            if (result.introPlaying) throw new Error("The intro did not acknowledge its native stop callback.");
            break;
        case "world-units":
        case "world-unit":
            var world = observe();
            if (world.phase !== "city" || !world.loadingFinalized || world.regionId !== id(packet.regionId) || world.cityId !== id(packet.cityId)) throw new Error("The requested Sandbox city is not the finalized active city.");
            var region = data("GetRegionDetails/" + id(packet.regionId));
            if (region.sandbox !== true && region.sandbox !== "true") throw new Error("World inspection requires Sandbox.");
            if (packet.op === "world-unit") {
                var reference = packet.argument;
                if (!/^(0|[1-9][0-9]{0,9}),(0|[1-9][0-9]{0,9})$/.test(reference)) throw new Error("Use an exact slot,stamp unit reference.");
                var referenceParts = reference.split(",");
                if (Number(referenceParts[0]) > 2147483647 || Number(referenceParts[1]) > 4294967295) throw new Error("Unit reference exceeds its native bounds.");
                result = { unitRef: reference, nativeUnitRef: data("units/" + reference + "/unitRef"), position: data("units/" + reference + "/position"), unitName: data("units/" + reference + "/unitName"), propKey: data("units/" + reference + "/propKey") };
            } else {
                var page = packet.argument.split(",");
                if (page.length !== 2 || !/^(0|[1-9][0-9]{0,5})$/.test(page[0]) || !/^[1-9][0-9]?$/.test(page[1]) || Number(page[1]) > 64) throw new Error("Use a bounded unit page: offset 0..999999, limit 1..64.");
                var references = Object.keys(data("units")).filter(function (value) {
                    if (!/^(0|[1-9][0-9]{0,9}),(0|[1-9][0-9]{0,9})$/.test(value)) return false;
                    var pair = value.split(",");
                    return Number(pair[0]) <= 2147483647 && Number(pair[1]) <= 4294967295;
                });
                references.sort();
                result = { regionId: world.regionId, cityId: world.cityId, cityUid: world.cityUid, total: references.length, offset: Number(page[0]), unitRefs: references.slice(Number(page[0]), Number(page[0]) + Number(page[1])) };
            }
            break;
        case "observe":
            result = observe();
            break;
        case "enter-menu":
            result = observe();
            if (result.phase !== "updater") break;
            if (result.updaterState !== 2 || result.blocked) throw new Error("The updater is not ready to enter the game; state=" + String(result.updaterState));
            client.RequestUpdaterData(2052759671);
            result = { queued: true, phase: "entering_menu" };
            break;
        case "region-templates":
            result = data("RegionTemplates");
            break;
        case "regions":
            result = data("PlayingRegions");
            break;
        case "region-details":
            result = data("GetRegionDetails/" + id(packet.regionId));
            break;
        case "create-sandbox-region":
            var templateId = id(packet.templateId);
            var templates = data("RegionTemplates").regionTemplates;
            var found = false;
            if (!templates) throw new Error("Region templates are not available.");
            for (var key in templates) {
                if (Object.prototype.hasOwnProperty.call(templates, key) && String(templates[key].key) === templateId && templates[key].status === "ACTIVE") found = true;
            }
            if (!found) throw new Error("The requested region template is not active.");
            result = data("CreateRegion/" + name(packet.regionName) + "/" + templateId + "/true");
            if (!result.id) throw new Error("The native region result lacks an identity; do not retry creation implicitly.");
            partial = { createdRegionId: String(result.id) };
            result = data("GetRegionDetails/" + id(result.id));
            if (result.sandbox !== true && result.sandbox !== "true") throw new Error("The created region did not confirm Sandbox; inspect its identity before retrying.");
            break;
        case "claim-city":
            var target = box(packet.regionId, packet.cityId);
            if (target.city.isClaimed === true || target.city.isClaimed === "true") throw new Error("The city is already claimed.");
            data("CreateBox/" + id(packet.regionId) + "/" + id(packet.cityId) + "/" + name(packet.cityName));
            partial = { regionId: id(packet.regionId), cityId: id(packet.cityId), claimMayHaveCompleted: true };
            result = box(packet.regionId, packet.cityId);
            if (result.city.isClaimed !== true && result.city.isClaimed !== "true") throw new Error("Native claim did not confirm ownership; do not retry implicitly.");
            break;
        case "load-city":
            var selected = box(packet.regionId, packet.cityId);
            if (selected.city.isClaimed !== true && selected.city.isClaimed !== "true") throw new Error("The requested city is not claimed.");
            if (typeof client.PostGameCommand !== "function") throw new Error("Game command bindings are not ready.");
            client.PostGameCommand("loadGame/" + id(packet.cityId));
            result = { queued: true, regionId: id(packet.regionId), cityId: id(packet.cityId), cityUid: String(selected.city.uid) };
            break;
        default:
            throw new Error("Unsupported native session operation.");
        }
        return JSON.stringify({ ok: true, operation: packet.op, result: result });
    } catch (error) {
        return JSON.stringify({ ok: false, partial: partial, error: String(error && error.message ? error.message : error) });
    }
})
