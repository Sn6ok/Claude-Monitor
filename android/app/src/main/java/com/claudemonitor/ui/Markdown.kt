package com.claudemonitor.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import com.claudemonitor.i18n.LocalStrings
import com.claudemonitor.i18n.Strings
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.claudemonitor.ui.theme.CalloutColors
import com.claudemonitor.ui.theme.MarkdownPalette
import com.claudemonitor.ui.theme.markdownPalette

/**
 * Показ тексту Claude з форматуванням, як у самому Claude Code — і трохи
 * наочніше: таблиці, чекбокси, виноски, підсвітка коду й змін.
 *
 * Розбір власний і навмисно вузький: це показ фрагмента на телефоні,
 * а не повноцінний рендерер Markdown. Бібліотека заради цього суперечила б
 * вимозі мінімуму залежностей (Частина 6 §39 Master Prompt). Усе, чого
 * розбір не впізнав, показується як звичайний текст — жоден символ
 * не зникає.
 */

// ── Розбір на блоки ──────────────────────────────────────────────────────────

sealed interface MdBlock {
    /** Блок коду в огорожі — з підсвіткою, або як зміни, якщо мова diff. */
    data class Code(val language: String?, val code: String) : MdBlock
    data class Heading(val level: Int, val text: String) : MdBlock
    data class Bullet(val indent: Int, val text: String) : MdBlock
    /** Пункт-чекбокс: `- [x] зроблено` чи `- [ ] ще ні`. */
    data class Task(val indent: Int, val checked: Boolean, val text: String) : MdBlock
    data class Numbered(val marker: String, val indent: Int, val text: String) : MdBlock
    data class Quote(val text: String) : MdBlock
    /** Виноска на кшталт `> [!WARNING]` — кольоровий блок за змістом. */
    data class Callout(val kind: CalloutKind, val text: String) : MdBlock
    data class Table(
        val header: List<String>,
        val align: List<TableAlign>,
        val rows: List<List<String>>,
    ) : MdBlock
    data class Paragraph(val text: String) : MdBlock
    data object Rule : MdBlock
}

enum class CalloutKind { NOTE, TIP, IMPORTANT, WARNING, CAUTION }

enum class TableAlign { START, CENTER, END }

// Межі таблиці: розбір працює над текстом з мережі, і таблиця
// на тисячі рядків не повинна вішати застосунок.
private const val MAX_TABLE_COLUMNS = 12
private const val MAX_TABLE_ROWS = 200

private val TABLE_SEPARATOR = Regex("""^\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?$""")
private val CALLOUT_MARKER = Regex("""^\[!(\w+)]\s*""")

/**
 * Розбирає текст на блоки.
 *
 * Функція чиста й не залежить від Compose — саме тому її можна перевіряти
 * тестами без емулятора.
 */
fun parseMarkdown(source: String): List<MdBlock> {
    val blocks = mutableListOf<MdBlock>()
    val lines = source.replace("\r\n", "\n").replace('\r', '\n').split('\n')
    val paragraph = StringBuilder()

    fun flushParagraph() {
        if (paragraph.isNotEmpty()) {
            blocks += MdBlock.Paragraph(paragraph.toString())
            paragraph.setLength(0)
        }
    }

    val fence = "```"
    var i = 0
    while (i < lines.size) {
        val line = lines[i].trimEnd()
        val trimmed = line.trimStart()
        val indent = line.length - trimmed.length

        // Блок коду. Текст події може бути обрізаний посеред блоку —
        // тоді закривальної огорожі немає, і блок доходить до кінця тексту.
        if (trimmed.startsWith(fence)) {
            flushParagraph()
            val language = trimmed.removePrefix(fence).trim().ifEmpty { null }
            val code = StringBuilder()
            i++
            while (i < lines.size && !lines[i].trimStart().startsWith(fence)) {
                if (code.isNotEmpty()) code.append('\n')
                code.append(lines[i])
                i++
            }
            i++  // пропускаємо закривальну огорожу, якщо вона є
            blocks += MdBlock.Code(language, code.toString())
            continue
        }

        if (trimmed.isEmpty()) {
            flushParagraph()
            i++
            continue
        }

        // Таблиця: рядок із вертикальними рисками, під ним — рядок-роздільник.
        // Без роздільника риски лишаються звичайним текстом.
        if (trimmed.startsWith("|") && i + 1 < lines.size && isTableSeparator(lines[i + 1].trim())) {
            flushParagraph()
            val header = splitTableRow(trimmed).take(MAX_TABLE_COLUMNS)
            val columns = header.size
            val separator = splitTableRow(lines[i + 1].trim()).map(::alignmentOf)
            val rows = mutableListOf<List<String>>()
            i += 2
            while (i < lines.size) {
                val row = lines[i].trim()
                if (!row.startsWith("|")) break
                if (rows.size < MAX_TABLE_ROWS) {
                    val cells = splitTableRow(row)
                    rows += List(columns) { cells.getOrElse(it) { "" } }
                }
                i++
            }
            blocks += MdBlock.Table(
                header = header,
                align = List(columns) { separator.getOrElse(it) { TableAlign.START } },
                rows = rows,
            )
            continue
        }

        // Горизонтальна лінія з трьох і більше однакових символів.
        if (trimmed.length >= 3 && trimmed.toSet().size == 1 &&
            (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '_')
        ) {
            flushParagraph()
            blocks += MdBlock.Rule
            i++
            continue
        }

        val heading = headingLevel(trimmed)
        if (heading > 0) {
            flushParagraph()
            blocks += MdBlock.Heading(heading, trimmed.drop(heading).trim())
            i++
            continue
        }

        // Цитата або виноска: суміжні рядки з «>» складають один блок.
        if (isQuoteLine(trimmed)) {
            flushParagraph()
            val quoteLines = mutableListOf<String>()
            while (i < lines.size) {
                val quoted = lines[i].trim()
                if (!isQuoteLine(quoted)) break
                quoteLines += quoted.removePrefix(">").removePrefix(" ")
                i++
            }
            blocks += quoteBlock(quoteLines)
            continue
        }

        if (isBullet(trimmed)) {
            flushParagraph()
            val content = trimmed.drop(2).trim()
            val checked = taskState(content)
            blocks += if (checked != null) {
                MdBlock.Task(indent, checked, content.drop(3).trim())
            } else {
                MdBlock.Bullet(indent, content)
            }
            i++
            continue
        }

        val numbered = numberedMarker(trimmed)
        if (numbered != null) {
            flushParagraph()
            blocks += MdBlock.Numbered(
                marker = numbered,
                indent = indent,
                text = trimmed.drop(numbered.length + 1).trim(),
            )
            i++
            continue
        }

        // Звичайний рядок. Переноси й відступ на початку зберігаються:
        // Claude розбиває думку на рядки свідомо, а у виводі команд
        // відступ показує вкладеність.
        if (paragraph.isNotEmpty()) paragraph.append('\n')
        paragraph.append(line)
        i++
    }

    flushParagraph()
    return blocks
}

private fun headingLevel(trimmed: String): Int {
    var level = 0
    while (level < trimmed.length && trimmed[level] == '#') level++
    // Потрібен пробіл після решіток, інакше «#1» стало б заголовком.
    if (level in 1..6 && level < trimmed.length && trimmed[level] == ' ') return level
    return 0
}

private fun isBullet(trimmed: String): Boolean {
    if (trimmed.length < 2) return false
    val marker = trimmed[0]
    if (marker != '-' && marker != '*' && marker != '+' && marker != '•') return false
    return trimmed[1] == ' '
}

/** Стан чекбокса на початку пункту: true, false або null, якщо це не чекбокс. */
private fun taskState(content: String): Boolean? {
    if (content.length < 3 || content[0] != '[' || content[2] != ']') return null
    // Після дужок — пробіл або кінець: «[x]посилання» чекбоксом не є.
    if (content.length > 3 && content[3] != ' ') return null
    return when (content[1]) {
        ' ' -> false
        'x', 'X' -> true
        else -> null
    }
}

/** Повертає маркер нумерованого пункту (наприклад «1.» чи «2)») або null. */
private fun numberedMarker(trimmed: String): String? {
    var digits = 0
    while (digits < trimmed.length && trimmed[digits].isDigit()) digits++
    if (digits == 0 || digits > 3) return null
    if (digits + 1 >= trimmed.length) return null
    val punctuation = trimmed[digits]
    if (punctuation != '.' && punctuation != ')') return null
    if (trimmed[digits + 1] != ' ') return null
    return trimmed.substring(0, digits + 1)
}

private fun isQuoteLine(trimmed: String): Boolean = trimmed.startsWith("> ") || trimmed == ">"

private fun quoteBlock(lines: List<String>): MdBlock {
    val first = lines.firstOrNull().orEmpty()
    val match = CALLOUT_MARKER.find(first)
    val kind = match?.let { calloutKindOf(it.groupValues[1]) }
    if (match != null && kind != null) {
        val rest = first.substring(match.range.last + 1).trim()
        val body = (listOf(rest) + lines.drop(1)).joinToString("\n").trim()
        return MdBlock.Callout(kind, body)
    }
    return MdBlock.Quote(lines.joinToString("\n").trim())
}

private fun calloutKindOf(name: String): CalloutKind? = when (name.uppercase()) {
    "NOTE", "INFO" -> CalloutKind.NOTE
    "TIP", "HINT", "SUCCESS" -> CalloutKind.TIP
    "IMPORTANT" -> CalloutKind.IMPORTANT
    "WARNING", "WARN", "ATTENTION" -> CalloutKind.WARNING
    "CAUTION", "DANGER", "ERROR" -> CalloutKind.CAUTION
    else -> null
}

private fun isTableSeparator(trimmed: String): Boolean =
    trimmed.contains('-') && TABLE_SEPARATOR.matches(trimmed)

/** Ділить рядок таблиці на клітинки. Екранована риска `\|` лишається всередині. */
private fun splitTableRow(row: String): List<String> {
    var body = row.trim()
    if (body.startsWith("|")) body = body.substring(1)
    if (body.endsWith("|") && !body.endsWith("\\|")) body = body.dropLast(1)

    val cells = ArrayList<String>()
    val cell = StringBuilder()
    var k = 0
    while (k < body.length) {
        val ch = body[k]
        if (ch == '\\' && k + 1 < body.length && body[k + 1] == '|') {
            cell.append('|')
            k += 2
            continue
        }
        if (ch == '|') {
            cells += cell.toString().trim()
            cell.setLength(0)
        } else {
            cell.append(ch)
        }
        k++
    }
    cells += cell.toString().trim()
    return cells
}

private fun alignmentOf(cell: String): TableAlign {
    val value = cell.trim()
    val left = value.startsWith(':')
    val right = value.endsWith(':')
    return when {
        left && right -> TableAlign.CENTER
        right -> TableAlign.END
        else -> TableAlign.START
    }
}

// ── Розбір усередині рядка ───────────────────────────────────────────────────

/** Кольори розмітки в рядку — передаються ззовні, щоб розбір не залежав від теми. */
data class MdColors(
    val code: Color,
    val codeBackground: Color,
    val link: Color,
    /** Колір жирного. Unspecified — такий самий, як у навколишнього тексту. */
    val strong: Color = Color.Unspecified,
)

/**
 * Перетворює рядок на текст зі стилями: жирний, курсив, код, закреслення,
 * посилання. Вкладеність підтримується, глибина обмежена — на випадок тексту
 * з десятками службових символів поспіль.
 */
fun buildInlineMarkdown(source: String, colors: MdColors): AnnotatedString =
    buildAnnotatedString { appendInline(source, colors, 0) }

private fun AnnotatedString.Builder.appendInline(src: String, colors: MdColors, depth: Int) {
    var i = 0
    val n = src.length

    while (i < n) {
        val ch = src[i]

        // Код у рядку розбирається першим: усередині нього розмітки немає.
        if (ch == '`') {
            val end = src.indexOf('`', i + 1)
            if (end > i + 1) {
                withStyle(
                    SpanStyle(
                        fontFamily = FontFamily.Monospace,
                        color = colors.code,
                        background = colors.codeBackground,
                    )
                ) {
                    append(src.substring(i + 1, end))
                }
                i = end + 1
                continue
            }
        }

        if (depth < 4) {
            if (ch == '*' && i + 1 < n && src[i + 1] == '*') {
                val end = src.indexOf("**", i + 2)
                if (end > i + 2) {
                    withStyle(SpanStyle(fontWeight = FontWeight.Bold, color = colors.strong)) {
                        appendInline(src.substring(i + 2, end), colors, depth + 1)
                    }
                    i = end + 2
                    continue
                }
            }

            if (ch == '~' && i + 1 < n && src[i + 1] == '~') {
                val end = src.indexOf("~~", i + 2)
                if (end > i + 2) {
                    withStyle(SpanStyle(textDecoration = TextDecoration.LineThrough)) {
                        appendInline(src.substring(i + 2, end), colors, depth + 1)
                    }
                    i = end + 2
                    continue
                }
            }

            if ((ch == '*' || ch == '_') && isEmphasisStart(src, i)) {
                val end = findEmphasisEnd(src, i + 1, ch)
                if (end > i + 1) {
                    withStyle(SpanStyle(fontStyle = FontStyle.Italic)) {
                        appendInline(src.substring(i + 1, end), colors, depth + 1)
                    }
                    i = end + 1
                    continue
                }
            }

            // Посилання виду [текст](адреса): показуємо текст зі стрілкою,
            // адресу приховуємо — на вузькому екрані вона витіснила б
            // саме повідомлення.
            if (ch == '[') {
                val close = src.indexOf(']', i + 1)
                if (close > i && close + 1 < n && src[close + 1] == '(') {
                    val paren = src.indexOf(')', close + 2)
                    if (paren > close) {
                        withStyle(SpanStyle(color = colors.link)) {
                            appendInline(src.substring(i + 1, close), colors, depth + 1)
                            append(" ↗")
                        }
                        i = paren + 1
                        continue
                    }
                }
            }
        }

        append(ch)
        i++
    }
}

private fun isEmphasisStart(src: String, i: Int): Boolean {
    if (i + 1 >= src.length) return false
    val next = src[i + 1]
    if (next.isWhitespace()) return false
    // Подвійний маркер сюди доходить лише тоді, коли пари для нього не знайшлося:
    // тоді це просто символи в тексті, а не курсив.
    if (next == src[i]) return false
    if (src[i] == '_') {
        // Назви на кшталт snake_case не є курсивом.
        val prev = if (i > 0) src[i - 1] else ' '
        if (prev.isLetterOrDigit() || prev == '_') return false
    }
    return true
}

private fun findEmphasisEnd(src: String, from: Int, marker: Char): Int {
    var j = from
    while (j < src.length) {
        if (src[j] == marker && !src[j - 1].isWhitespace()) {
            if (marker == '_') {
                val next = if (j + 1 < src.length) src[j + 1] else ' '
                if (next.isLetterOrDigit() || next == '_') {
                    j++
                    continue
                }
            }
            return j
        }
        j++
    }
    return -1
}

// ── Показ ────────────────────────────────────────────────────────────────────

/** Текст Claude із розміткою. */
@Composable
fun MarkdownText(
    text: String,
    modifier: Modifier = Modifier,
    baseStyle: TextStyle = MaterialTheme.typography.bodyMedium,
    baseColor: Color = MaterialTheme.colorScheme.onSurface,
) {
    MarkdownBlocks(text, modifier, baseStyle, baseColor, depth = 0)
}

// Виноска може містити розмітку, зокрема іншу виноску. Глибина обмежена,
// щоб текст із сотнею вкладених виносок не переповнив стек.
private const val MAX_NESTING = 2

@Composable
private fun MarkdownBlocks(
    text: String,
    modifier: Modifier,
    baseStyle: TextStyle,
    baseColor: Color,
    depth: Int,
) {
    val palette = markdownPalette()
    val inline = MdColors(
        code = palette.inlineCode,
        codeBackground = palette.inlineCodeBg,
        link = palette.link,
        strong = palette.strong,
    )
    val blocks = remember(text) { parseMarkdown(text) }

    Column(modifier) {
        blocks.forEachIndexed { index, block ->
            if (index > 0) Spacer(Modifier.height(spacingBefore(block)))
            when (block) {
                is MdBlock.Code ->
                    if (isDiffLanguage(block.language)) DiffBlock(block, palette) else CodeBlock(block, palette)

                is MdBlock.Heading -> HeadingBlock(block, baseStyle, palette, inline)

                is MdBlock.Bullet -> ListItem(block.indent, marker = {
                    BulletMarker(listLevel(block.indent), palette)
                }) {
                    Text(buildInlineMarkdown(block.text, inline), style = baseStyle, color = baseColor)
                }

                is MdBlock.Task -> ListItem(block.indent, marker = {
                    TaskMarker(block.checked, palette)
                }) {
                    // Виконаний пункт приглушено й закреслено: погляд одразу
                    // падає на те, що ще лишилось зробити.
                    Text(
                        text = buildInlineMarkdown(block.text, inline),
                        style = if (block.checked) {
                            baseStyle.copy(textDecoration = TextDecoration.LineThrough)
                        } else {
                            baseStyle
                        },
                        color = if (block.checked) palette.taskDoneText else baseColor,
                    )
                }

                is MdBlock.Numbered -> ListItem(block.indent, marker = {
                    Text(
                        text = block.marker,
                        style = baseStyle,
                        fontWeight = FontWeight.SemiBold,
                        color = palette.numbered,
                    )
                }) {
                    Text(buildInlineMarkdown(block.text, inline), style = baseStyle, color = baseColor)
                }

                is MdBlock.Quote -> Text(
                    text = buildInlineMarkdown(block.text, inline),
                    modifier = Modifier
                        .fillMaxWidth()
                        .leftBar(palette.quoteBar)
                        .padding(start = 13.dp),
                    style = baseStyle.copy(fontStyle = FontStyle.Italic),
                    color = palette.quoteText,
                )

                is MdBlock.Callout -> CalloutBlock(block, baseStyle, palette, inline, depth)

                is MdBlock.Table -> TableBlock(block, palette, inline)

                MdBlock.Rule -> HorizontalDivider(
                    Modifier.padding(vertical = 4.dp),
                    color = palette.rule,
                )

                is MdBlock.Paragraph -> Text(
                    text = buildInlineMarkdown(block.text, inline),
                    style = baseStyle,
                    color = baseColor,
                )
            }
        }
    }
}

/** Великим блокам — більше повітря навколо, рядкам списку — менше. */
private fun spacingBefore(block: MdBlock): Dp = when (block) {
    is MdBlock.Code, is MdBlock.Table, is MdBlock.Callout, is MdBlock.Heading -> 8.dp
    else -> 3.dp
}

private fun listLevel(indent: Int): Int = when {
    indent <= 0 -> 0
    indent <= 4 -> 1
    else -> 2
}

/**
 * Смужка ліворуч — для заголовків, цитат і виносок.
 *
 * Малюється за вмістом, а не окремим елементом із висотою «як у тексту»:
 * вимірювання внутрішніх розмірів ламається, щойно всередині опиняється
 * блок, що сам залежить від ширини екрана.
 */
private fun Modifier.leftBar(color: Color, inset: Dp = 0.dp): Modifier = drawBehind {
    val top = inset.toPx()
    val height = (size.height - 2 * top).coerceAtLeast(0f)
    drawRect(color, topLeft = Offset(0f, top), size = Size(3.dp.toPx(), height))
}

@Composable
private fun ListItem(
    indent: Int,
    marker: @Composable () -> Unit,
    content: @Composable () -> Unit,
) {
    Row(Modifier.padding(start = (listLevel(indent) * 16).dp)) {
        Box(Modifier.widthIn(min = 18.dp).padding(end = 4.dp)) { marker() }
        Box(Modifier.weight(1f)) { content() }
    }
}

/** Маркер пункту: коло, порожнє коло, квадрат — щоб рівні вкладеності розрізнялись. */
@Composable
private fun BulletMarker(level: Int, palette: MarkdownPalette) {
    val color = palette.bullets[level.coerceIn(0, palette.bullets.lastIndex)]
    val shape = if (level >= 2) RoundedCornerShape(1.dp) else CircleShape
    // Відступ згори ставить маркер на рівень середини першого рядка тексту.
    val base = Modifier.padding(top = 7.dp).size(6.dp)
    Box(if (level == 1) base.border(1.2.dp, color, shape) else base.background(color, shape))
}

/** Чекбокс малюється вручну: символи ☑ і ☐ є не в кожному шрифті телефона. */
@Composable
private fun TaskMarker(checked: Boolean, palette: MarkdownPalette) {
    val color = if (checked) palette.taskDone else palette.taskTodo
    Canvas(Modifier.padding(top = 3.dp).size(14.dp)) {
        val stroke = 1.4.dp.toPx()
        val radius = CornerRadius(3.dp.toPx())
        if (checked) {
            drawRoundRect(color = color, cornerRadius = radius)
            val tick = Path().apply {
                moveTo(size.width * 0.26f, size.height * 0.52f)
                lineTo(size.width * 0.44f, size.height * 0.70f)
                lineTo(size.width * 0.76f, size.height * 0.32f)
            }
            drawPath(
                path = tick,
                color = Color.White,
                style = Stroke(width = stroke * 1.2f, cap = StrokeCap.Round, join = StrokeJoin.Round),
            )
        } else {
            val half = stroke / 2
            drawRoundRect(
                color = color,
                topLeft = Offset(half, half),
                size = Size(size.width - stroke, size.height - stroke),
                cornerRadius = radius,
                style = Stroke(width = stroke),
            )
        }
    }
}

@Composable
private fun HeadingBlock(
    block: MdBlock.Heading,
    baseStyle: TextStyle,
    palette: MarkdownPalette,
    inline: MdColors,
) {
    val scale = when (block.level) {
        1 -> 1.25f
        2 -> 1.15f
        else -> 1.05f
    }
    val style = baseStyle.copy(fontWeight = FontWeight.Bold, fontSize = baseStyle.fontSize * scale)
    val text = buildInlineMarkdown(block.text, inline.copy(strong = palette.heading))

    // Смужка — лише в заголовків верхніх рівнів: вони ділять відповідь
    // на розділи, а дрібні підзаголовки й так не губляться.
    val modifier = if (block.level <= 2) {
        Modifier.leftBar(palette.headingBar, inset = 3.dp).padding(start = 11.dp)
    } else {
        Modifier
    }
    Text(text = text, modifier = modifier, style = style, color = palette.heading)
}

@Composable
private fun CalloutBlock(
    block: MdBlock.Callout,
    baseStyle: TextStyle,
    palette: MarkdownPalette,
    inline: MdColors,
    depth: Int,
) {
    val colors = calloutColors(block.kind, palette)
    val title = calloutTitle(block.kind, LocalStrings.current)
    val inlineHere = inline.copy(strong = colors.text)
    val shortBody = !block.text.contains('\n')

    Column(
        Modifier
            .fillMaxWidth()
            .background(colors.background)
            .leftBar(colors.bar)
            .padding(start = 13.dp, end = 10.dp, top = 6.dp, bottom = 6.dp)
    ) {
        if (shortBody || depth >= MAX_NESTING) {
            // Коротка виноска — одним рядком: «Увага · текст».
            Text(
                text = buildAnnotatedString {
                    withStyle(SpanStyle(fontWeight = FontWeight.SemiBold)) { append(title) }
                    if (block.text.isNotEmpty()) {
                        append("  ·  ")
                        append(buildInlineMarkdown(block.text, inlineHere))
                    }
                },
                style = baseStyle,
                color = colors.text,
            )
        } else {
            Text(
                text = title,
                style = baseStyle.copy(fontWeight = FontWeight.SemiBold),
                color = colors.text,
            )
            Spacer(Modifier.height(2.dp))
            // Довга виноска може містити списки й код — розбираємо її так само.
            MarkdownBlocks(block.text, Modifier, baseStyle, colors.text, depth + 1)
        }
    }
}

private fun calloutColors(kind: CalloutKind, palette: MarkdownPalette): CalloutColors = when (kind) {
    CalloutKind.NOTE -> palette.note
    CalloutKind.TIP -> palette.tip
    CalloutKind.IMPORTANT -> palette.important
    CalloutKind.WARNING -> palette.warning
    CalloutKind.CAUTION -> palette.caution
}

private fun calloutTitle(kind: CalloutKind, strings: Strings): String = when (kind) {
    CalloutKind.NOTE -> strings.calloutNote
    CalloutKind.TIP -> strings.calloutTip
    CalloutKind.IMPORTANT -> strings.calloutImportant
    CalloutKind.WARNING -> strings.calloutWarning
    CalloutKind.CAUTION -> strings.calloutCaution
}

@Composable
private fun TableBlock(block: MdBlock.Table, palette: MarkdownPalette, inline: MdColors) {
    val widths = remember(block) { tableColumnWidths(block) }
    val divider = 0.5.dp
    val totalWidth = widths.fold(0.dp) { acc, width -> acc + width } + divider * (widths.size - 1)
    val cellStyle = MaterialTheme.typography.bodySmall
    val shape = RoundedCornerShape(6.dp)

    // Широку таблицю гортають убік, а не стискають до нечитабельних колонок.
    Box(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState())) {
        Column(Modifier.clip(shape).border(divider, palette.tableBorder, shape)) {
            TableRow(
                cells = block.header,
                widths = widths,
                align = block.align,
                style = cellStyle.copy(fontWeight = FontWeight.SemiBold),
                background = palette.tableHeaderBg,
                textColor = palette.tableHeaderText,
                border = palette.tableBorder,
                inline = inline.copy(strong = palette.tableHeaderText),
            )
            block.rows.forEach { row ->
                Box(Modifier.width(totalWidth).height(divider).background(palette.tableBorder))
                TableRow(
                    cells = row,
                    widths = widths,
                    align = block.align,
                    style = cellStyle,
                    background = palette.tableCellBg,
                    textColor = MaterialTheme.colorScheme.onSurface,
                    border = palette.tableBorder,
                    inline = inline,
                )
            }
        }
    }
}

@Composable
private fun TableRow(
    cells: List<String>,
    widths: List<Dp>,
    align: List<TableAlign>,
    style: TextStyle,
    background: Color,
    textColor: Color,
    border: Color,
    inline: MdColors,
) {
    // Однакова висота клітинок у рядку — інакше тло й розділювачі
    // стали б сходинками.
    Row(Modifier.height(IntrinsicSize.Min).background(background)) {
        cells.forEachIndexed { index, cell ->
            if (index > 0) Box(Modifier.width(0.5.dp).fillMaxHeight().background(border))
            Text(
                text = buildInlineMarkdown(cell, inline),
                modifier = Modifier
                    .width(widths.getOrElse(index) { 48.dp })
                    .padding(horizontal = 8.dp, vertical = 5.dp),
                style = style,
                color = textColor,
                textAlign = when (align.getOrElse(index) { TableAlign.START }) {
                    TableAlign.START -> TextAlign.Start
                    TableAlign.CENTER -> TextAlign.Center
                    TableAlign.END -> TextAlign.End
                },
            )
        }
    }
}

/** Ширина колонки за найдовшим вмістом — у межах, щоб одна клітинка не розтягла все. */
private fun tableColumnWidths(table: MdBlock.Table): List<Dp> =
    List(table.header.size) { column ->
        val longest = (listOf(table.header) + table.rows).maxOf { row ->
            row.getOrElse(column) { "" }.count { it != '*' && it != '`' && it != '_' && it != '~' }
        }
        (longest * 7 + 18).coerceIn(48, 240).dp
    }

/** Шапка блоку коду: мова ліворуч, кнопка копіювання праворуч. */
@Composable
private fun CodeHeader(label: String, background: Color, color: Color, onCopy: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().background(background).padding(start = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            fontFamily = FontFamily.Monospace,
            color = color,
            modifier = Modifier.weight(1f),
        )
        CopyButton(onClick = onCopy, size = 28.dp, color = color)
    }
}

@Composable
private fun CodeBlock(block: MdBlock.Code, palette: MarkdownPalette) {
    val context = LocalContext.current
    val strings = LocalStrings.current
    val highlighted = remember(block.code, block.language, palette.syntax) {
        highlightCode(block.code, block.language, palette.syntax)
    }

    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(6.dp)).background(palette.codeBg)) {
        CodeHeader(
            label = block.language ?: strings.codeLabel,
            background = palette.codeHeaderBg,
            color = palette.codeHeaderText,
            onCopy = { copyToClipboard(context, block.code, strings.copied) },
        )
        // Код не переносимо, а прокручуємо: перенесений відступ ламає
        // структуру, за якою код і читають.
        Text(
            text = highlighted,
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState())
                .padding(horizontal = 10.dp, vertical = 8.dp),
            style = MaterialTheme.typography.bodySmall.copy(
                fontFamily = FontFamily.Monospace,
                fontSize = 12.sp,
            ),
            color = palette.syntax.plain,
            softWrap = false,
        )
    }
}

/** Зміни коду: видалене — червоним, додане — зеленим. */
@Composable
private fun DiffBlock(block: MdBlock.Code, palette: MarkdownPalette) {
    val context = LocalContext.current
    val strings = LocalStrings.current
    val lines = remember(block.code) { block.code.split('\n') }
    val shape = RoundedCornerShape(6.dp)

    Column(
        Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(palette.diffBg)
            .border(0.5.dp, palette.tableBorder, shape)
    ) {
        CodeHeader(
            label = block.language ?: "diff",
            background = palette.diffHeaderBg,
            color = palette.diffHeaderText,
            onCopy = { copyToClipboard(context, block.code, strings.copied) },
        )
        BoxWithConstraints(Modifier.fillMaxWidth()) {
            val viewport = maxWidth
            Box(Modifier.horizontalScroll(rememberScrollState())) {
                // Тло рядка — на всю ширину: і коли рядок коротший за екран,
                // і коли довший і його гортають.
                Column(
                    Modifier
                        .widthIn(min = viewport)
                        .width(IntrinsicSize.Max)
                        .padding(vertical = 4.dp)
                ) {
                    lines.forEach { line ->
                        val (background, color) = when (diffLineKind(line)) {
                            DiffLine.ADDED -> palette.diffAddedBg to palette.diffAddedText
                            DiffLine.REMOVED -> palette.diffRemovedBg to palette.diffRemovedText
                            DiffLine.HUNK -> palette.diffHunkBg to palette.diffHunkText
                            DiffLine.META -> Color.Transparent to palette.diffHeaderText
                            DiffLine.CONTEXT -> Color.Transparent to palette.diffContextText
                        }
                        Text(
                            text = line.ifEmpty { " " },
                            modifier = Modifier
                                .fillMaxWidth()
                                .background(background)
                                .padding(horizontal = 10.dp, vertical = 1.dp),
                            style = MaterialTheme.typography.bodySmall.copy(
                                fontFamily = FontFamily.Monospace,
                                fontSize = 12.sp,
                            ),
                            color = color,
                            softWrap = false,
                        )
                    }
                }
            }
        }
    }
}
