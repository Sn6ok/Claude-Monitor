package com.claudemonitor.data

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.PublicKey
import java.security.Signature
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import java.security.interfaces.ECPublicKey
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec
import java.math.BigInteger
import java.security.MessageDigest
import java.security.SecureRandom

/**
 * Криптографія застосунку.
 *
 * Повне дзеркало реалізації Bridge (bridge/src/crypto.cpp), інакше сторони
 * просто не зрозуміли б одна одну. Набір алгоритмів обрано так, щоб обидві
 * платформи користувалися ВБУДОВАНИМИ засобами й не тягнули криптобібліотек
 * (Частина 3 §25 Master Prompt):
 *
 *   підпис        ECDSA P-256 + SHA-256, формат IEEE P1363 (сирі r||s)
 *   узгодження    ECDH P-256
 *   похідні ключі HKDF-SHA256 (RFC 5869)
 *   шифрування    AES-256-GCM
 *
 * Приватний ключ підпису живе в Android Keystore і НІКОЛИ не залишає
 * пристрій — за наявності апаратного модуля він недоступний навіть
 * самому застосунку (Частина 4 §15 Master Prompt).
 */
object Crypto {

    private const val KEYSTORE = "AndroidKeyStore"
    const val ALIAS_SIGNING = "claude_monitor_signing_v1"
    const val ALIAS_EXCHANGE = "claude_monitor_exchange_v1"

    /** Довжина неспресованої точки P-256: 0x04 || X(32) || Y(32). */
    const val EC_POINT_SIZE = 65
    const val SIGNATURE_SIZE = 64
    const val GCM_NONCE_SIZE = 12
    const val GCM_TAG_BITS = 128

    private val secureRandom = SecureRandom()

    // ── Ключі ────────────────────────────────────────────────────────────────

    private fun keyStore(): KeyStore =
        KeyStore.getInstance(KEYSTORE).apply { load(null) }

    fun hasKeys(): Boolean {
        val store = keyStore()
        return store.containsAlias(ALIAS_SIGNING) && store.containsAlias(ALIAS_EXCHANGE)
    }

    /**
     * Створює обидві пари ключів у Keystore.
     *
     * Ключі свідомо НЕ вимагають автентифікації користувача для кожної
     * операції: монітор має перепідключатися у фоні, і запит відбитка
     * пальця на кожен кадр зробив би застосунок непридатним.
     */
    fun generateKeys() {
        generateKey(ALIAS_SIGNING, KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY)
        generateKey(ALIAS_EXCHANGE, KeyProperties.PURPOSE_AGREE_KEY)
    }

    private fun generateKey(alias: String, purposes: Int) {
        val generator = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_EC, KEYSTORE)
        val builder = KeyGenParameterSpec.Builder(alias, purposes)
            .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
            .setUserAuthenticationRequired(false)

        if (purposes and KeyProperties.PURPOSE_SIGN != 0) {
            builder.setDigests(KeyProperties.DIGEST_SHA256)
        }

        generator.initialize(builder.build())
        generator.generateKeyPair()
    }

    fun deleteKeys() {
        val store = keyStore()
        runCatching { store.deleteEntry(ALIAS_SIGNING) }
        runCatching { store.deleteEntry(ALIAS_EXCHANGE) }
    }

    private fun privateKey(alias: String): PrivateKey =
        keyStore().getKey(alias, null) as PrivateKey

    private fun publicKey(alias: String): PublicKey =
        keyStore().getCertificate(alias).publicKey

    /**
     * Публічний ключ у вигляді сирої неспресованої точки — саме такий
     * формат передається в протоколі й розуміє Windows CNG.
     */
    fun publicPoint(alias: String): ByteArray {
        val key = publicKey(alias) as ECPublicKey
        val point = key.w
        val out = ByteArray(EC_POINT_SIZE)
        out[0] = 0x04

        // Координати можуть мати провідний нуль-байт від BigInteger або,
        // навпаки, бути коротшими за 32 байти. Вирівнюємо праворуч.
        writeFixed(point.affineX, out, 1)
        writeFixed(point.affineY, out, 33)
        return out
    }

    private fun writeFixed(value: BigInteger, out: ByteArray, offset: Int) {
        val bytes = value.toByteArray()
        when {
            bytes.size == 32 -> System.arraycopy(bytes, 0, out, offset, 32)
            bytes.size == 33 && bytes[0] == 0.toByte() ->
                System.arraycopy(bytes, 1, out, offset, 32)
            bytes.size < 32 ->
                System.arraycopy(bytes, 0, out, offset + (32 - bytes.size), bytes.size)
            else ->
                System.arraycopy(bytes, bytes.size - 32, out, offset, 32)
        }
    }

    /** Перетворює сиру точку на об'єкт публічного ключа. */
    fun publicKeyFromPoint(raw: ByteArray): PublicKey? {
        if (raw.size != EC_POINT_SIZE || raw[0] != 0x04.toByte()) return null
        return runCatching {
            val x = BigInteger(1, raw.copyOfRange(1, 33))
            val y = BigInteger(1, raw.copyOfRange(33, 65))

            // Параметри кривої беремо з власного ключа: так не доводиться
            // задавати константи secp256r1 вручну.
            val params = (publicKey(ALIAS_EXCHANGE) as ECPublicKey).params
            val factory = KeyFactory.getInstance("EC")
            factory.generatePublic(ECPublicKeySpec(ECPoint(x, y), params))
        }.getOrNull()
    }

    // ── Підпис ───────────────────────────────────────────────────────────────

    /**
     * Підписує повідомлення ключем із Keystore.
     *
     * Формат "SHA256withECDSAinP1363Format" дає сирі r||s — рівно те,
     * що повертає BCryptSignHash на боці Bridge. Стандартний DER довелося б
     * конвертувати вручну на обох платформах.
     */
    fun sign(message: String): ByteArray {
        val data = message.toByteArray(Charsets.UTF_8)

        // Основний шлях: підпис одразу у форматі IEEE P1363 (сирі r‖s).
        runCatching {
            val signature = Signature.getInstance("SHA256withECDSAinP1363Format")
            signature.initSign(privateKey(ALIAS_SIGNING))
            signature.update(data)
            signature.sign()
        }.onSuccess { return it }

        // Запасний шлях: не всі пристрої надають P1363 для ключів
        // з Keystore. Тоді підписуємо стандартним ECDSA, який повертає DER,
        // і переводимо результат у P1363 самі.
        val der = Signature.getInstance("SHA256withECDSA").run {
            initSign(privateKey(ALIAS_SIGNING))
            update(data)
            sign()
        }
        return derToP1363(der)
            ?: throw IllegalStateException("не вдалося перетворити підпис у формат P1363")
    }

    /**
     * Переводить підпис ECDSA з DER у IEEE P1363.
     *
     * DER: SEQUENCE { INTEGER r, INTEGER s } — числа записані зі змінною
     * довжиною і можливим провідним нулем. P1363 очікує рівно 32 + 32 байти,
     * тому кожне число вирівнюється праворуч.
     */
    fun derToP1363(der: ByteArray): ByteArray? {
        try {
            var pos = 0
            if (der[pos++] != 0x30.toByte()) return null

            // Довжина послідовності: коротка або довга форма.
            var length = der[pos++].toInt() and 0xFF
            if (length and 0x80 != 0) {
                val lengthBytes = length and 0x7F
                length = 0
                repeat(lengthBytes) { length = (length shl 8) or (der[pos++].toInt() and 0xFF) }
            }

            fun readInteger(): ByteArray? {
                if (der[pos++] != 0x02.toByte()) return null
                val size = der[pos++].toInt() and 0xFF
                val value = der.copyOfRange(pos, pos + size)
                pos += size
                return value
            }

            val r = readInteger() ?: return null
            val s = readInteger() ?: return null

            val out = ByteArray(SIGNATURE_SIZE)
            copyRightAligned(r, out, 0)
            copyRightAligned(s, out, 32)
            return out
        } catch (e: Exception) {
            return null
        }
    }

    private fun copyRightAligned(value: ByteArray, out: ByteArray, offset: Int) {
        // Провідні нулі DER відкидаємо, коротші числа доповнюємо зліва.
        var start = 0
        while (start < value.size - 1 && value[start] == 0.toByte()) start++
        val length = value.size - start
        val target = offset + (32 - minOf(length, 32))
        System.arraycopy(value, start + maxOf(0, length - 32), out, target, minOf(length, 32))
    }

    // ── Узгодження ключа ─────────────────────────────────────────────────────

    /**
     * Обчислює спільний ключ наскрізного шифрування.
     *
     * Сіль — обидва ідентифікатори пристроїв у сталому порядку, тому обидві
     * сторони отримують однаковий ключ незалежно від того, хто рахує
     * (docs/protocol.md §5).
     */
    fun deriveSessionKey(peerExchangePoint: ByteArray,
                         myDeviceId: String,
                         peerDeviceId: String): ByteArray? {
        val peerKey = publicKeyFromPoint(peerExchangePoint) ?: return null

        val salt = if (myDeviceId < peerDeviceId) myDeviceId + peerDeviceId
                   else peerDeviceId + myDeviceId

        // ECDH через Keystore доступний не на всіх пристроях: PURPOSE_AGREE_KEY
        // з'явився в API 31, а деякі виробники реалізують його неповно.
        // Тому невдача тут не повинна валити застосунок — вона просто
        // означає, що ключ обчислити не вдалося.
        val shared = runCatching {
            val agreement = KeyAgreement.getInstance("ECDH", KEYSTORE)
            agreement.init(privateKey(ALIAS_EXCHANGE))
            agreement.doPhase(peerKey, true)
            agreement.generateSecret()
        }.getOrElse { keystoreError ->
            // Запасний шлях: та сама операція без прив'язки до Keystore.
            // Спрацьовує, коли постачальник Keystore не підтримує ECDH.
            runCatching {
                val agreement = KeyAgreement.getInstance("ECDH")
                agreement.init(privateKey(ALIAS_EXCHANGE))
                agreement.doPhase(peerKey, true)
                agreement.generateSecret()
            }.getOrElse {
                android.util.Log.e(
                    "Crypto",
                    "ECDH недоступний: ${keystoreError.javaClass.simpleName}",
                )
                return null
            }
        }

        return runCatching {
            hkdf(shared, salt.toByteArray(Charsets.UTF_8),
                 "claude-monitor-v1/e2ee".toByteArray(Charsets.UTF_8), 32)
        }.getOrNull()
    }

    // ── HKDF (RFC 5869) ──────────────────────────────────────────────────────

    fun hkdf(ikm: ByteArray, salt: ByteArray, info: ByteArray, length: Int): ByteArray {
        // Крок 1: extract.
        val extractMac = Mac.getInstance("HmacSHA256")
        val effectiveSalt = if (salt.isEmpty()) ByteArray(32) else salt
        extractMac.init(SecretKeySpec(effectiveSalt, "HmacSHA256"))
        val prk = extractMac.doFinal(ikm)

        // Крок 2: expand.
        val out = ByteArray(length)
        var previous = ByteArray(0)
        var produced = 0
        var counter = 1

        while (produced < length) {
            val mac = Mac.getInstance("HmacSHA256")
            mac.init(SecretKeySpec(prk, "HmacSHA256"))
            mac.update(previous)
            mac.update(info)
            mac.update(counter.toByte())
            previous = mac.doFinal()

            val chunk = minOf(previous.size, length - produced)
            System.arraycopy(previous, 0, out, produced, chunk)
            produced += chunk
            counter += 1
        }
        return out
    }

    fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        return mac.doFinal(data)
    }

    fun sha256(data: ByteArray): ByteArray =
        MessageDigest.getInstance("SHA-256").digest(data)

    // ── AES-256-GCM ──────────────────────────────────────────────────────────

    /**
     * Nonce: 4 байти напрямку + 8 байтів лічильника.
     *
     * Повторне використання nonce з тим самим ключем розкрило б відкритий
     * текст, тому лічильник монотонний і ніколи не йде на друге коло.
     */
    class NonceCounter(private val direction: String) {
        // Ключ між з'єднаннями сталий, тож нумерація не може щоразу починатися
        // з нуля: повтор nonce з тим самим ключем розкриває відкритий текст.
        // Старт — поточний час у мілісекундах, помножений на 1024: телефон
        // надсилає лише кілька запитів за з'єднання, тож діапазони сусідніх
        // з'єднань не перетинаються.
        private var counter: Long = System.currentTimeMillis() * 1024

        fun next(): ByteArray {
            val nonce = ByteArray(GCM_NONCE_SIZE)
            val dir = direction.toByteArray(Charsets.US_ASCII)
            System.arraycopy(dir, 0, nonce, 0, minOf(4, dir.size))

            var value = counter++
            for (i in 7 downTo 0) {
                nonce[4 + i] = (value and 0xFF).toByte()
                value = value ushr 8
            }
            return nonce
        }
    }

    fun encrypt(key: ByteArray, nonce: ByteArray, plaintext: String): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"),
                    GCMParameterSpec(GCM_TAG_BITS, nonce))
        return cipher.doFinal(plaintext.toByteArray(Charsets.UTF_8))
    }

    /**
     * Дешифрує та перевіряє тег автентичності.
     * @return null за будь-якої невідповідності — і за пошкодження,
     *         і за спроби підміни. Обидва випадки однаково неприйнятні.
     */
    fun decrypt(key: ByteArray, nonce: ByteArray, ciphertextWithTag: ByteArray): String? =
        runCatching {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"),
                        GCMParameterSpec(GCM_TAG_BITS, nonce))
            String(cipher.doFinal(ciphertextWithTag), Charsets.UTF_8)
        }.getOrNull()

    // ── Допоміжне ────────────────────────────────────────────────────────────

    fun randomBytes(length: Int): ByteArray =
        ByteArray(length).also { secureRandom.nextBytes(it) }

    fun deviceIdFrom(publicPoint: ByteArray): String = sha256(publicPoint).toHex()

    fun ByteArray.toHex(): String {
        val hex = "0123456789abcdef"
        val out = StringBuilder(size * 2)
        for (byte in this) {
            val value = byte.toInt() and 0xFF
            out.append(hex[value shr 4]).append(hex[value and 0x0F])
        }
        return out.toString()
    }

    // Base64url реалізовано власноруч, а не через android.util.Base64.
    //
    // Причина практична: код протоколу має піддаватися звичайним юніт-тестам
    // на JVM, а класи з android.util там недоступні. Заодно це прибирає
    // залежність від поведінки конкретної версії Android.
    private const val B64_ALPHABET =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

    fun ByteArray.toBase64Url(): String {
        val out = StringBuilder((size + 2) / 3 * 4)
        var i = 0

        while (i + 2 < size) {
            val triple = ((this[i].toInt() and 0xFF) shl 16) or
                ((this[i + 1].toInt() and 0xFF) shl 8) or
                (this[i + 2].toInt() and 0xFF)
            out.append(B64_ALPHABET[(triple shr 18) and 0x3F])
            out.append(B64_ALPHABET[(triple shr 12) and 0x3F])
            out.append(B64_ALPHABET[(triple shr 6) and 0x3F])
            out.append(B64_ALPHABET[triple and 0x3F])
            i += 3
        }

        // Без символів доповнення: саме такий формат очікує протокол.
        when (size - i) {
            1 -> {
                val triple = (this[i].toInt() and 0xFF) shl 16
                out.append(B64_ALPHABET[(triple shr 18) and 0x3F])
                out.append(B64_ALPHABET[(triple shr 12) and 0x3F])
            }
            2 -> {
                val triple = ((this[i].toInt() and 0xFF) shl 16) or
                    ((this[i + 1].toInt() and 0xFF) shl 8)
                out.append(B64_ALPHABET[(triple shr 18) and 0x3F])
                out.append(B64_ALPHABET[(triple shr 12) and 0x3F])
                out.append(B64_ALPHABET[(triple shr 6) and 0x3F])
            }
        }
        return out.toString()
    }

    fun String.fromBase64Url(): ByteArray? {
        if (isEmpty()) return ByteArray(0)

        val out = java.io.ByteArrayOutputStream(length * 3 / 4 + 3)
        var buffer = 0
        var bits = 0

        for (c in this) {
            val value = when (c) {
                in 'A'..'Z' -> c - 'A'
                in 'a'..'z' -> c - 'a' + 26
                in '0'..'9' -> c - '0' + 52
                '-' -> 62
                '_' -> 63
                // Сторонній символ означає зіпсований або підроблений вхід.
                else -> return null
            }
            buffer = (buffer shl 6) or value
            bits += 6
            if (bits >= 8) {
                bits -= 8
                out.write((buffer shr bits) and 0xFF)
            }
        }
        return out.toByteArray()
    }
}
