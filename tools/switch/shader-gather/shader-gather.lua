-- name: Shader Gather
-- description: Warps through every level area with each cap power, so the Switch build compiles (and records in shadercache/combiners.txt) the color combiners they use. Used by tools/switch/gather_combiners.sh.

local SCENES = {
    { LEVEL_CASTLE_GROUNDS, 1 }, { LEVEL_CASTLE, 1 }, { LEVEL_CASTLE, 2 }, { LEVEL_CASTLE, 3 },
    { LEVEL_CASTLE_COURTYARD, 1 },
    { LEVEL_BOB, 1 }, { LEVEL_WF, 1 }, { LEVEL_JRB, 1 }, { LEVEL_JRB, 2 }, { LEVEL_CCM, 1 }, { LEVEL_CCM, 2 },
    { LEVEL_BBH, 1 }, { LEVEL_HMC, 1 }, { LEVEL_LLL, 1 }, { LEVEL_LLL, 2 },
    { LEVEL_SSL, 1 }, { LEVEL_SSL, 2 }, { LEVEL_SSL, 3 }, { LEVEL_DDD, 1 }, { LEVEL_DDD, 2 },
    { LEVEL_SL, 1 }, { LEVEL_SL, 2 }, { LEVEL_WDW, 1 }, { LEVEL_WDW, 2 },
    { LEVEL_TTM, 1 }, { LEVEL_TTM, 2 }, { LEVEL_TTM, 3 }, { LEVEL_TTM, 4 },
    { LEVEL_THI, 1 }, { LEVEL_THI, 2 }, { LEVEL_THI, 3 }, { LEVEL_TTC, 1 }, { LEVEL_RR, 1 },
    { LEVEL_BITDW, 1 }, { LEVEL_BITFS, 1 }, { LEVEL_BITS, 1 },
    { LEVEL_BOWSER_1, 1 }, { LEVEL_BOWSER_2, 1 }, { LEVEL_BOWSER_3, 1 },
    { LEVEL_PSS, 1 }, { LEVEL_COTMC, 1 }, { LEVEL_TOTWC, 1 }, { LEVEL_VCUTM, 1 }, { LEVEL_WMOTR, 1 }, { LEVEL_SA, 1 },
}

-- each scene is visited once per cap: metal and vanish Mario use their own combiners
local CAPS = { 0, MARIO_METAL_CAP, MARIO_VANISH_CAP, MARIO_WING_CAP }

local SCENE_FRAMES = 30 * 4

local timer = 0
local step = 0

hook_event(HOOK_UPDATE, function()
    if not network_is_server() then return end
    timer = timer + 1

    local m = gMarioStates[0]
    local cap = CAPS[(step - 1) % #CAPS + 1]
    if step > 0 then
        m.flags = (m.flags & ~(MARIO_METAL_CAP | MARIO_VANISH_CAP | MARIO_WING_CAP)) | cap
        m.capTimer = 600
    end

    if step == 0 and timer == 30 * 3 or step > 0 and timer == SCENE_FRAMES then
        step = step + 1
        timer = 0
        if step > #SCENES * #CAPS then
            print("SHADERGATHER done")
            step = 0
            timer = -1000000
            return
        end
        local scene = SCENES[(step - 1) // #CAPS + 1]
        print("SHADERGATHER scene " .. step .. " level " .. scene[1] .. " area " .. scene[2])
        if (step - 1) % #CAPS == 0 then
            warp_to_level(scene[1], scene[2], 1)
        end
    end
end)
