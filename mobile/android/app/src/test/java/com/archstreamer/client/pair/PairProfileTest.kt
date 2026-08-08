package com.archstreamer.client.pair

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class PairProfileTest {
    @Test
    fun jsonRoundTripKeepsPassword() {
        val original = PairProfile(
            host = "192.168.1.10",
            altHost = "10.0.6.1",
            controlPort = "45555",
            inputPort = "45454",
            username = "merk",
            password = "secret",
            streamQuality = 2,
            streamBitrate = 2,
            streamSize = 2,
            streamFeel = 0,
        )
        val parsed = PairProfile.fromJson(original.toJson())
        assertEquals(original, parsed)
    }

    @Test
    fun parsePairUri() {
        val target = PairTarget.parseUri(
            "archstreamer://pair?v=1&ip=192.168.1.20&port=41234&token=abcdef0123456789",
        )
        assertEquals("192.168.1.20", target.ip)
        assertEquals(41234, target.port)
        assertEquals("abcdef0123456789", target.token)
    }

    @Test
    fun rejectBadUri() {
        val err = runCatching { PairTarget.parseUri("https://example.com") }
        assertTrue(err.isFailure)
    }
}
