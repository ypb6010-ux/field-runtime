-- bcd_datetime.lua — decode/encode a PLC RTC clock stored as BCD across four
-- holding registers, to/from a "YYYY-MM-DD hh:mm:ss" string.
--
-- BCD (binary-coded decimal): each nibble is one decimal digit, so the byte
-- 0x24 means decimal 24, NOT 0x24 (=36). PLC real-time-clock registers are
-- almost always laid out this way; the builtin scalar pipeline can't express it.
--
-- Register layout (1-indexed, high byte first within each word):
--   raw[1] = 0x00YY              year  (00..99, +2000)
--   raw[2] = 0xMMDD              month, day
--   raw[3] = 0xhhmm              hour, minute
--   raw[4] = 0xss00              second
--
-- `ctx` (unused here) carries { address, count, scale, offset }.

local function bcd2dec(b) return math.floor(b / 16) * 10 + (b % 16) end
local function dec2bcd(d) return math.floor(d / 10) * 16 + (d % 10) end
local function hi(w) return math.floor(w / 256) % 256 end
local function lo(w) return w % 256 end

return {
    decode = function(raw, ctx)
        if #raw < 4 then return nil end
        local year   = 2000 + bcd2dec(lo(raw[1]))
        local month  = bcd2dec(hi(raw[2]))
        local day    = bcd2dec(lo(raw[2]))
        local hour   = bcd2dec(hi(raw[3]))
        local minute = bcd2dec(lo(raw[3]))
        local second = bcd2dec(hi(raw[4]))
        return string.format("%04d-%02d-%02d %02d:%02d:%02d",
                             year, month, day, hour, minute, second)
    end,

    encode = function(value, ctx)
        -- value: "YYYY-MM-DD hh:mm:ss"
        local Y, Mo, D, h, mi, s =
            value:match("(%d+)-(%d+)-(%d+)%s+(%d+):(%d+):(%d+)")
        if not Y then return {} end
        local r1 = dec2bcd(tonumber(Y) % 100)
        local r2 = dec2bcd(tonumber(Mo)) * 256 + dec2bcd(tonumber(D))
        local r3 = dec2bcd(tonumber(h))  * 256 + dec2bcd(tonumber(mi))
        local r4 = dec2bcd(tonumber(s))  * 256
        return { r1, r2, r3, r4 }
    end,
}
