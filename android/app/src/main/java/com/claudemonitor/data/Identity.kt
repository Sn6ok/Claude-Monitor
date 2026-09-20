package com.claudemonitor.data

import android.content.Context
import android.content.SharedPreferences
import com.claudemonitor.data.Crypto.toBase64Url
import com.claudemonitor.data.Crypto.fromBase64Url

/**
 * Криптографічна ідентичність застосунку та дані спареного ноутбука.
 *
 * Що де зберігається (Частина 3 §5, Частина 4 §16 Master Prompt):
 *
 *   приватні ключі   Android Keystore — не залишають пристрій ніколи,
 *                    за наявності апаратного модуля недоступні навіть
 *                    самому застосунку;
 *   публічні дані    звичайні налаштування: адреса Relay, публічні ключі
 *                    ноутбука, ідентифікатори. Це не секрети.
 *
 * У сховищі немає й не буде: облікових даних Claude, ключів API,
 * вихідного коду, транскриптів, паролів.
 */
class Identity(context: Context) {

    private val prefs: SharedPreferences =
        context.getSharedPreferences("claude_monitor", Context.MODE_PRIVATE)

    var relayUrl: String
        get() = prefs.getString(KEY_RELAY_URL, "") ?: ""
        set(value) = prefs.edit().putString(KEY_RELAY_URL, value.trim()).apply()

    var peerDeviceId: String
        get() = prefs.getString(KEY_PEER_ID, "") ?: ""
        private set(value) = prefs.edit().putString(KEY_PEER_ID, value).apply()

    var peerPublicSign: String
        get() = prefs.getString(KEY_PEER_SIGN, "") ?: ""
        private set(value) = prefs.edit().putString(KEY_PEER_SIGN, value).apply()

    var peerPublicExchange: String
        get() = prefs.getString(KEY_PEER_ECDH, "") ?: ""
        private set(value) = prefs.edit().putString(KEY_PEER_ECDH, value).apply()

    val isPaired: Boolean
        get() = peerDeviceId.isNotEmpty() && Crypto.hasKeys()

    /** Ідентифікатор цього пристрою — SHA-256 від публічної точки підпису. */
    val deviceId: String
        get() {
            ensureKeys()
            return Crypto.deviceIdFrom(Crypto.publicPoint(Crypto.ALIAS_SIGNING))
        }

    val publicSign: String
        get() {
            ensureKeys()
            return Crypto.publicPoint(Crypto.ALIAS_SIGNING).toBase64Url()
        }

    val publicExchange: String
        get() {
            ensureKeys()
            return Crypto.publicPoint(Crypto.ALIAS_EXCHANGE).toBase64Url()
        }

    fun ensureKeys() {
        if (!Crypto.hasKeys()) Crypto.generateKeys()
    }

    /**
     * Зберігає дані ноутбука після успішного pairing.
     * @return true, якщо дані записано
     */
    fun savePeer(pubSign: String, pubExchange: String): Boolean {
        val point = pubSign.fromBase64Url() ?: return false
        if (point.size != Crypto.EC_POINT_SIZE) return false

        return runCatching {
            peerDeviceId = Crypto.deviceIdFrom(point)
            peerPublicSign = pubSign
            peerPublicExchange = pubExchange
            true
        }.getOrDefault(false)
    }

    /**
     * Забуває ноутбук і знищує ключі.
     *
     * Викликається, коли Relay повідомляє, що пристрій відкликано:
     * тримати непотрібні ключі немає підстав, а нове підключення все одно
     * потребує повторного pairing (Частина 3 §21 Master Prompt).
     */
    fun forgetPeer(deleteKeys: Boolean = false) {
        prefs.edit()
            .remove(KEY_PEER_ID)
            .remove(KEY_PEER_SIGN)
            .remove(KEY_PEER_ECDH)
            .apply()
        if (deleteKeys) Crypto.deleteKeys()
    }

    /** Спільний ключ наскрізного шифрування. null, доки немає пари. */
    fun sessionKey(): ByteArray? = runCatching {
        if (!isPaired) return null
        val peerPoint = peerPublicExchange.fromBase64Url() ?: return null
        Crypto.deriveSessionKey(peerPoint, deviceId, peerDeviceId)
    }.getOrNull()

    private companion object {
        const val KEY_RELAY_URL = "relay_url"
        const val KEY_PEER_ID = "peer_device_id"
        const val KEY_PEER_SIGN = "peer_pub_sign"
        const val KEY_PEER_ECDH = "peer_pub_ecdh"
    }
}
