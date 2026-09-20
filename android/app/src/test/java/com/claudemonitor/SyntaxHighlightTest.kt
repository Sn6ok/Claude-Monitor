package com.claudemonitor

import com.claudemonitor.ui.DiffLine
import com.claudemonitor.ui.TokenKind
import com.claudemonitor.ui.diffLineKind
import com.claudemonitor.ui.tokenizeCode
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Тести лексера підсвітки.
 *
 * Головне тут — не «красиво», а «не бреше»: помилкова підсвітка (незакритий
 * рядок, що фарбує пів блоку, чи ключове слово всередині імені) читається
 * гірше, ніж код без кольорів.
 */
class SyntaxHighlightTest {

    private fun tokens(code: String, language: String?): List<Pair<String, TokenKind>> =
        tokenizeCode(code, language).map { code.substring(it.start, it.end) to it.kind }

    @Test
    fun `ключове слово число й коментар у kotlin`() {
        assertEquals(
            listOf("val" to TokenKind.KEYWORD, "42" to TokenKind.NUMBER, "// лічильник" to TokenKind.COMMENT),
            tokens("val count = 42 // лічильник", "kotlin"),
        )
    }

    @Test
    fun `ключове слово лише як ціле слово`() {
        assertTrue(tokens("valid = interval", "kotlin").isEmpty())
    }

    @Test
    fun `тип з великої літери`() {
        assertEquals(
            listOf("val" to TokenKind.KEYWORD, "String" to TokenKind.TYPE),
            tokens("val s: String", "kotlin"),
        )
    }

    @Test
    fun `рядок з екранованою лапкою — один токен`() {
        val code = "\"a\\\"b\""
        assertEquals(listOf(code to TokenKind.STRING), tokens(code, "kotlin"))
    }

    @Test
    fun `незакритий рядок не фарбує наступні рядки`() {
        assertEquals(
            listOf("\"abc" to TokenKind.STRING, "val" to TokenKind.KEYWORD),
            tokens("\"abc\nval x", "kotlin"),
        )
    }

    @Test
    fun `цифри в імені не є числом`() {
        assertTrue(tokens("x42 = y7", "kotlin").isEmpty())
    }

    @Test
    fun `незакритий блоковий коментар тягнеться до кінця`() {
        assertEquals(listOf("/* b\nc" to TokenKind.COMMENT), tokens("a /* b\nc", "cpp"))
    }

    @Test
    fun `директива препроцесора`() {
        assertEquals("#include" to TokenKind.KEYWORD, tokens("#include <vector>", "cpp").first())
    }

    @Test
    fun `декоратор і коментар у python`() {
        assertEquals(
            listOf("@cache" to TokenKind.TYPE, "def" to TokenKind.KEYWORD, "# кеш" to TokenKind.COMMENT),
            tokens("@cache\ndef f(): # кеш", "python"),
        )
    }

    @Test
    fun `змінна й коментар у bash`() {
        assertEquals(
            listOf("echo" to TokenKind.KEYWORD, "\$HOME" to TokenKind.VARIABLE, "# дім" to TokenKind.COMMENT),
            tokens("echo \$HOME # дім", "bash"),
        )
    }

    @Test
    fun `решітка в адресі не є коментарем`() {
        assertTrue(tokens("curl https://example.org/#top", "bash").none { it.second == TokenKind.COMMENT })
    }

    @Test
    fun `шлях зі зворотною рискою в powershell не ламає рядок`() {
        // У PowerShell «\» — частина шляху, а не екранування. Інакше лапка
        // після build\ не закривала б рядок, і командлет лишився б без кольору.
        val result = tokens("cd \"D:\\Wall\\build\\\" ; Get-ChildItem", "powershell")

        assertEquals("\"D:\\Wall\\build\\\"" to TokenKind.STRING, result[1])
        assertEquals("Get-ChildItem" to TokenKind.TYPE, result.last())
    }

    @Test
    fun `ключі й значення в json`() {
        assertEquals(
            listOf(
                "\"n\"" to TokenKind.KEY, "5" to TokenKind.NUMBER,
                "\"ok\"" to TokenKind.KEY, "true" to TokenKind.KEYWORD,
                "\"s\"" to TokenKind.KEY, "\"x\"" to TokenKind.STRING,
            ),
            tokens("{\"n\": 5, \"ok\": true, \"s\": \"x\"}", "json"),
        )
    }

    @Test
    fun `невідома мова лишається без підсвітки`() {
        assertTrue(tokenizeCode("val x = 1", null).isEmpty())
        assertTrue(tokenizeCode("val x = 1", "text").isEmpty())
    }

    @Test
    fun `лексер витримує код із самих лапок і коментарів`() {
        val code = "\"".repeat(1000) + "/*".repeat(500) + "'".repeat(1000)
        tokenizeCode(code, "kotlin")  // не зависає й не падає
    }

    @Test
    fun `види рядків diff`() {
        assertEquals(DiffLine.ADDED, diffLineKind("+ val limit = 3072"))
        assertEquals(DiffLine.REMOVED, diffLineKind("- val limit = 512"))
        assertEquals(DiffLine.HUNK, diffLineKind("@@ -1,3 +1,3 @@"))
        assertEquals(DiffLine.META, diffLineKind("+++ b/common.h"))
        assertEquals(DiffLine.META, diffLineKind("--- a/common.h"))
        assertEquals(DiffLine.CONTEXT, diffLineKind(" незмінений рядок"))
    }
}
