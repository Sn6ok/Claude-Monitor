package com.claudemonitor.ui

import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import com.claudemonitor.ui.theme.SyntaxColors

/**
 * Підсвітка синтаксису в блоках коду.
 *
 * Це не повноцінний розбір мов, а легкий лексер на один прохід: ключові
 * слова, рядки, числа, коментарі, типи. Щоб прочитати фрагмент на телефоні,
 * цього досить, а бібліотека підсвітки важила б більше за весь застосунок
 * (Частина 6 §39 Master Prompt).
 *
 * Мова, якої лексер не знає, лишається без підсвітки: неправильні кольори
 * заважали б читати більше, ніж їхня відсутність.
 */

enum class TokenKind { KEYWORD, STRING, NUMBER, COMMENT, TYPE, VARIABLE, KEY }

/** Ділянка коду, яку треба підсвітити: від [start] включно до [end] виключно. */
data class CodeToken(val start: Int, val end: Int, val kind: TokenKind)

private enum class Family { C_LIKE, PYTHON, SHELL, POWERSHELL, JSON, NONE }

private fun familyOf(language: String?): Family = when (language?.trim()?.lowercase()) {
    "kotlin", "kt", "kts", "java", "c", "h", "cpp", "c++", "cc", "cxx", "hpp", "hxx",
    "cs", "csharp", "js", "javascript", "jsx", "mjs", "cjs", "ts", "typescript", "tsx",
    "go", "golang", "rust", "rs", "swift", "dart", "scala", "groovy", "gradle" -> Family.C_LIKE
    "python", "py" -> Family.PYTHON
    "bash", "sh", "shell", "zsh" -> Family.SHELL
    "powershell", "ps1", "ps", "pwsh" -> Family.POWERSHELL
    "json", "jsonc", "json5" -> Family.JSON
    else -> Family.NONE
}

// Лише справжні ключові слова. М'які на кшталт data, value, type лишаються
// поза списком: їх часто вживають як звичайні імена, і фарбувати їх як
// ключові слова було б неправдою.
private val C_KEYWORDS = hashSetOf(
    "abstract", "as", "async", "await", "auto", "bool", "boolean", "break", "byte", "case",
    "catch", "char", "class", "companion", "const", "constexpr", "continue", "crossinline",
    "default", "defer", "delete", "do", "double", "else", "enum", "explicit", "export",
    "extends", "extern", "external", "false", "final", "finally", "float", "fn", "for",
    "friend", "fun", "func", "function", "goto", "if", "impl", "implements", "import", "in",
    "infix", "inline", "instanceof", "int", "interface", "internal", "is", "lateinit", "let",
    "long", "mut", "namespace", "native", "new", "nil", "noexcept", "noinline", "null",
    "nullptr", "object", "open", "operator", "override", "package", "private", "protected",
    "pub", "public", "readonly", "register", "reified", "return", "sealed", "self", "short",
    "signed", "sizeof", "static", "struct", "super", "suspend", "switch", "synchronized",
    "tailrec", "template", "this", "throw", "throws", "trait", "transient", "true", "try",
    "typealias", "typedef", "typename", "typeof", "undefined", "union", "unsigned", "using",
    "val", "var", "vararg", "virtual", "void", "volatile", "when", "while", "with", "yield",
)

private val PYTHON_KEYWORDS = hashSetOf(
    "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "False", "finally", "for", "from", "global", "if", "import",
    "in", "is", "lambda", "None", "nonlocal", "not", "or", "pass", "raise", "return", "True",
    "try", "while", "with", "yield", "self",
)

private val SHELL_KEYWORDS = hashSetOf(
    "if", "then", "else", "elif", "fi", "for", "foreach", "while", "until", "do", "done",
    "case", "esac", "in", "function", "return", "local", "export", "readonly", "declare",
    "exit", "break", "continue", "source", "echo", "cd", "set", "unset", "sudo", "param",
    "try", "catch", "finally", "throw", "switch", "trap", "shift", "eval", "exec",
    "true", "false",
)

private val JSON_KEYWORDS = hashSetOf("true", "false", "null")

private fun keywordsOf(family: Family): Set<String> = when (family) {
    Family.C_LIKE -> C_KEYWORDS
    Family.PYTHON -> PYTHON_KEYWORDS
    Family.SHELL, Family.POWERSHELL -> SHELL_KEYWORDS
    Family.JSON -> JSON_KEYWORDS
    Family.NONE -> emptySet()
}

private const val NUMBER_CHARS = "0123456789abcdefABCDEFxXoO_.lLuU"

/**
 * Розбиває код на ділянки для підсвітки. Звичайний текст у результат не входить.
 *
 * Функція чиста й від Compose не залежить, тож перевіряється звичайними тестами.
 */
fun tokenizeCode(code: String, language: String?): List<CodeToken> {
    val family = familyOf(language)
    if (family == Family.NONE) return emptyList()

    val shell = family == Family.SHELL || family == Family.POWERSHELL
    val keywords = keywordsOf(family)
    val tokens = ArrayList<CodeToken>()
    val n = code.length
    var i = 0

    fun lineEnd(from: Int): Int = code.indexOf('\n', from).let { if (it < 0) n else it }

    while (i < n) {
        val c = code[i]

        // ── Коментарі ────────────────────────────────────────────────────
        if (family == Family.C_LIKE && c == '/' && i + 1 < n && code[i + 1] == '/') {
            val end = lineEnd(i)
            tokens += CodeToken(i, end, TokenKind.COMMENT)
            i = end
            continue
        }
        if (family == Family.C_LIKE && c == '/' && i + 1 < n && code[i + 1] == '*') {
            val close = code.indexOf("*/", i + 2)
            val end = if (close < 0) n else close + 2
            tokens += CodeToken(i, end, TokenKind.COMMENT)
            i = end
            continue
        }
        // Решітка — коментар лише на початку слова: у https://site/#top це частина адреси.
        if ((family == Family.PYTHON || shell) && c == '#' && (i == 0 || code[i - 1].isWhitespace())) {
            val end = lineEnd(i)
            tokens += CodeToken(i, end, TokenKind.COMMENT)
            i = end
            continue
        }

        // ── Рядки ────────────────────────────────────────────────────────
        val quote = c == '"' || c == '\'' || (c == '`' && family == Family.C_LIKE)
        if (quote) {
            val triple = c == '"' && (family == Family.C_LIKE || family == Family.PYTHON) &&
                code.startsWith("\"\"\"", i) ||
                c == '\'' && family == Family.PYTHON && code.startsWith("'''", i)
            val end = when {
                triple -> code.indexOf(code.substring(i, i + 3), i + 3).let { if (it < 0) n else it + 3 }
                // У PowerShell зворотна риска — звичайний символ шляху, а не
                // екранування: інакше "D:\build\" поглинав би решту рядка.
                else -> scanQuoted(code, i, c, multiline = c == '`', escapes = family != Family.POWERSHELL)
            }
            val kind = if (family == Family.JSON && c == '"' && followedByColon(code, end)) {
                TokenKind.KEY
            } else {
                TokenKind.STRING
            }
            tokens += CodeToken(i, end, kind)
            i = end
            continue
        }

        // ── Змінні оболонки: $HOME, ${name}, $env:PATH ───────────────────
        if (shell && c == '$' && i + 1 < n && (isIdentStart(code[i + 1]) || code[i + 1] == '{')) {
            var j = i + 1
            if (code[j] == '{') {
                val close = code.indexOf('}', j)
                j = if (close < 0) n else close + 1
            } else {
                while (j < n && (isIdentPart(code[j]) || code[j] == ':')) j++
            }
            tokens += CodeToken(i, j, TokenKind.VARIABLE)
            i = j
            continue
        }

        // ── Числа — лише окремим словом: у x42 цифри належать імені ──────
        if (c.isDigit() && (i == 0 || !isIdentPart(code[i - 1]))) {
            var j = i + 1
            while (j < n && NUMBER_CHARS.indexOf(code[j]) >= 0) j++
            tokens += CodeToken(i, j, TokenKind.NUMBER)
            i = j
            continue
        }

        // ── Анотації й декоратори: @Composable, @dataclass ───────────────
        if ((family == Family.C_LIKE || family == Family.PYTHON) && c == '@' &&
            i + 1 < n && isIdentStart(code[i + 1])
        ) {
            var j = i + 1
            while (j < n && isIdentPart(code[j])) j++
            tokens += CodeToken(i, j, TokenKind.TYPE)
            i = j
            continue
        }

        // ── Директиви препроцесора C/C++: #include, #define ─────────────
        if (family == Family.C_LIKE && c == '#' && atLineStart(code, i) &&
            i + 1 < n && isIdentStart(code[i + 1])
        ) {
            var j = i + 1
            while (j < n && isIdentPart(code[j])) j++
            tokens += CodeToken(i, j, TokenKind.KEYWORD)
            i = j
            continue
        }

        // ── Слова ────────────────────────────────────────────────────────
        if (isIdentStart(c)) {
            var j = i + 1
            while (j < n && isIdentPart(code[j])) j++

            // Командлети PowerShell пишуться через дефіс: Get-ChildItem.
            if (shell && c.isUpperCase() && j + 1 < n && code[j] == '-' && code[j + 1].isUpperCase()) {
                var k = j + 1
                while (k < n && isIdentPart(code[k])) k++
                tokens += CodeToken(i, k, TokenKind.TYPE)
                i = k
                continue
            }

            val word = code.substring(i, j)
            val kind = when {
                word in keywords -> TokenKind.KEYWORD
                family == Family.C_LIKE && c.isUpperCase() -> TokenKind.TYPE
                else -> null
            }
            if (kind != null) tokens += CodeToken(i, j, kind)
            i = j
            continue
        }

        i++
    }

    return tokens
}

/** Код із підсвіткою для показу. */
fun highlightCode(code: String, language: String?, colors: SyntaxColors): AnnotatedString {
    val tokens = tokenizeCode(code, language)
    if (tokens.isEmpty()) return AnnotatedString(code)

    return buildAnnotatedString {
        append(code)
        for (token in tokens) {
            val style = when (token.kind) {
                TokenKind.KEYWORD -> SpanStyle(color = colors.keyword)
                TokenKind.STRING -> SpanStyle(color = colors.string)
                TokenKind.NUMBER -> SpanStyle(color = colors.number)
                TokenKind.COMMENT -> SpanStyle(color = colors.comment, fontStyle = FontStyle.Italic)
                TokenKind.TYPE -> SpanStyle(color = colors.type)
                TokenKind.VARIABLE -> SpanStyle(color = colors.variable)
                TokenKind.KEY -> SpanStyle(color = colors.key)
            }
            addStyle(style, token.start, token.end)
        }
    }
}

private fun isIdentStart(c: Char) = c.isLetter() || c == '_'

private fun isIdentPart(c: Char) = c.isLetterOrDigit() || c == '_'

/**
 * Кінець рядка в лапках. Незакритий рядок обривається на кінці рядка коду —
 * одна забута лапка не повинна фарбувати весь блок.
 */
private fun scanQuoted(code: String, start: Int, quote: Char, multiline: Boolean, escapes: Boolean): Int {
    var j = start + 1
    while (j < code.length) {
        val ch = code[j]
        if (escapes && ch == '\\') {
            j += 2
            continue
        }
        if (ch == quote) return j + 1
        if (ch == '\n' && !multiline) return j
        j++
    }
    return code.length
}

private fun followedByColon(code: String, from: Int): Boolean {
    var j = from
    while (j < code.length && (code[j] == ' ' || code[j] == '\t')) j++
    return j < code.length && code[j] == ':'
}

private fun atLineStart(code: String, index: Int): Boolean {
    var j = index - 1
    while (j >= 0 && code[j] != '\n') {
        if (!code[j].isWhitespace()) return false
        j--
    }
    return true
}

// ── Зміни коду (diff) ────────────────────────────────────────────────────────

enum class DiffLine { ADDED, REMOVED, HUNK, META, CONTEXT }

fun isDiffLanguage(language: String?): Boolean =
    language?.trim()?.lowercase().let { it == "diff" || it == "patch" }

/** Вид рядка diff. Заголовки файлів (+++, ---) — не зміни, а службові рядки. */
fun diffLineKind(line: String): DiffLine = when {
    line.startsWith("+++") || line.startsWith("---") -> DiffLine.META
    line.startsWith("@@") -> DiffLine.HUNK
    line.startsWith("+") -> DiffLine.ADDED
    line.startsWith("-") -> DiffLine.REMOVED
    else -> DiffLine.CONTEXT
}
