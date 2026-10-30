-- name: Player Bots
-- description: Load test: every player except the host circles around the host's Mario and jumps, so joined test clients cost what real players cost. Used for the Switch many-player test.

local RADIUS = 400
local TELEPORT_DIST = 2500

local frame = 0

hook_event(HOOK_BEFORE_MARIO_UPDATE, function(m)
    if m.playerIndex ~= 0 or network_is_server() then return end
    frame = frame + 1

    local hostIndex = network_local_index_from_global(0)
    if hostIndex == nil or hostIndex == 0 then return end
    local host = gMarioStates[hostIndex]
    local np = gNetworkPlayers[hostIndex]
    local me = gNetworkPlayers[0]
    if not np.connected or np.currLevelNum ~= me.currLevelNum or np.currAreaIndex ~= me.currAreaIndex then return end

    -- a point on a circle around the host, spread out by player index
    local angle = (frame * 200 + me.globalIndex * 0x10000 // 8) & 0xFFFF
    local tx = host.pos.x + sins(angle) * RADIUS
    local tz = host.pos.z + coss(angle) * RADIUS
    local dx, dz = tx - m.pos.x, tz - m.pos.z

    if dx * dx + dz * dz > TELEPORT_DIST * TELEPORT_DIST or math.abs(host.pos.y - m.pos.y) > TELEPORT_DIST then
        m.pos.x, m.pos.y, m.pos.z = tx, host.pos.y + 200, tz
        m.vel.x, m.vel.y, m.vel.z = 0, 0, 0
        set_mario_action(m, ACT_FREEFALL, 0)
        return
    end

    -- intendedYaw = atan2s(-stickY, stickX) + camera yaw
    local camYaw = (m.area ~= nil and m.area.camera ~= nil) and m.area.camera.yaw or 0
    local rel = atan2s(dz, dx) - camYaw
    m.controller.stickX = sins(rel) * 64
    m.controller.stickY = -coss(rel) * 64
    m.controller.stickMag = 64

    if frame % 90 == 0 then
        m.controller.buttonPressed = m.controller.buttonPressed | A_BUTTON
        m.controller.buttonDown = m.controller.buttonDown | A_BUTTON
    end
end)
