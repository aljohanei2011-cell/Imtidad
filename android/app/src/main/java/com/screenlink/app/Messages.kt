package com.screenlink.app

import org.json.JSONObject

/** Builders for the control-channel messages in shared/protocol.md. */
object Messages {
    const val PROTOCOL_VERSION = 1

    fun hello(id: String = "", device: String = ""): String =
        JSONObject().put("type", "HELLO").put("client", "android").put("version", PROTOCOL_VERSION)
            .put("id", id).put("device", device).toString()

    fun auth(pin: String, id: String, device: String): String =
        JSONObject().put("type", "AUTH").put("pin", pin).put("id", id).put("device", device).toString()

    /** A paired device proves it holds its key: proof = HMAC-SHA256(key, nonce + id), in hex. */
    fun authProof(proof: String, id: String, device: String): String =
        JSONObject().put("type", "AUTH").put("proof", proof).put("id", id).put("device", device).toString()

    fun clip(text: String): String = JSONObject().put("type", "CLIP").put("text", text).toString()

    /** A control-panel button: name = key | combo | lock | sleep | restart | shutdown | open. */
    fun action(name: String, build: JSONObject.() -> Unit = {}): String =
        JSONObject().put("type", "ACTION").put("name", name).apply(build).toString()

    fun ping(): String = JSONObject().put("type", "PING").toString()

    fun setMode(mode: String): String = JSONObject().put("type", "SET_MODE").put("mode", mode).toString()
}
