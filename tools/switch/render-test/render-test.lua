-- name: Render Test
-- description: Warps through a fixed set of scenes and prints a marker in each, so tools/switch/render_compare.sh can capture the same frames on PC and Switch.

local SCENES = {
    { name = "castle_grounds", level = LEVEL_CASTLE_GROUNDS, area = 1 },
    { name = "bob",            level = LEVEL_BOB,            area = 1 },
    { name = "ccm_slide",      level = LEVEL_CCM,            area = 2 },
    { name = "bitdw",          level = LEVEL_BITDW,          area = 1 },
}

local SETTLE_FRAMES = 30 * 6  -- after a warp, wait for the camera to settle
local SCENE_FRAMES  = 30 * 40 -- in yuzu the marker can reach the host ~10 s late

local timer = 0
local scene = 0

hook_event(HOOK_UPDATE, function()
    if not network_is_server() then return end
    timer = timer + 1

    if scene == 0 and timer == 30 * 3 or scene > 0 and timer == SCENE_FRAMES then
        scene = scene + 1
        timer = 0
        if scene > #SCENES then
            print("RENDERTEST done")
            return
        end
        warp_to_level(SCENES[scene].level, SCENES[scene].area, 1)
    elseif scene > 0 and scene <= #SCENES and timer == SETTLE_FRAMES then
        print("RENDERTEST ready " .. SCENES[scene].name)
    end
end)
