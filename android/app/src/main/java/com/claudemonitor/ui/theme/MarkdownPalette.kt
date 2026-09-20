package com.claudemonitor.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.luminance

/**
 * Кольори розмітки у відповідях Claude.
 *
 * Кожен колір несе зміст, а не прикрашає: заголовок, код, успіх, увага,
 * помилка. Для світлої й темної теми — окремі набори з тим самим змістом:
 * колір, що добре читається на білому, на темному тлі губиться, і навпаки.
 *
 * Набір обирається за яскравістю поточного тла, а не за перемикачем теми.
 * Тож коли в налаштуваннях з'явиться власна тема, розмітка підлаштується
 * під неї сама.
 */
data class MarkdownPalette(
    val heading: Color,
    val headingBar: Color,
    val strong: Color,
    val inlineCode: Color,
    val inlineCodeBg: Color,
    val link: Color,
    /** Маркери списків за рівнем вкладеності: перший, другий, третій і глибше. */
    val bullets: List<Color>,
    val numbered: Color,
    val taskDone: Color,
    val taskTodo: Color,
    val taskDoneText: Color,
    val quoteBar: Color,
    val quoteText: Color,
    val rule: Color,
    val tableBorder: Color,
    val tableHeaderBg: Color,
    val tableHeaderText: Color,
    val tableCellBg: Color,
    val codeBg: Color,
    val codeHeaderBg: Color,
    val codeHeaderText: Color,
    val syntax: SyntaxColors,
    val diffHeaderBg: Color,
    val diffHeaderText: Color,
    val diffBg: Color,
    val diffContextText: Color,
    val diffAddedBg: Color,
    val diffAddedText: Color,
    val diffRemovedBg: Color,
    val diffRemovedText: Color,
    val diffHunkBg: Color,
    val diffHunkText: Color,
    val note: CalloutColors,
    val tip: CalloutColors,
    val important: CalloutColors,
    val warning: CalloutColors,
    val caution: CalloutColors,
)

/** Кольори однієї виноски: тло, смужка ліворуч і текст. */
data class CalloutColors(val background: Color, val bar: Color, val text: Color)

/**
 * Кольори підсвітки коду.
 *
 * Блок коду має темне тло в обох темах — так підсвітка лишається однаково
 * контрастною, і набір кольорів потрібен лише один.
 */
data class SyntaxColors(
    val plain: Color,
    val keyword: Color,
    val string: Color,
    val number: Color,
    val comment: Color,
    val type: Color,
    val variable: Color,
    val key: Color,
)

private val CodeSyntax = SyntaxColors(
    plain = Color(0xFFE8E8EA),
    keyword = Color(0xFFED93B1),
    string = Color(0xFF97C459),
    number = Color(0xFFFAC775),
    comment = Color(0xFF888780),
    type = Color(0xFF85B7EB),
    variable = Color(0xFF5DCAA5),
    key = Color(0xFF85B7EB),
)

val LightMarkdownPalette = MarkdownPalette(
    heading = Color(0xFF9A3F22),
    headingBar = Color(0xFFC96442),
    strong = Color(0xFF000000),
    inlineCode = Color(0xFFA3302E),
    inlineCodeBg = Color(0xFFF6E3E1),
    link = Color(0xFF185FA5),
    bullets = listOf(Color(0xFFC96442), Color(0xFF1D9E75), Color(0xFF888780)),
    numbered = Color(0xFFC96442),
    taskDone = Color(0xFF3B6D11),
    taskTodo = Color(0xFF888780),
    taskDoneText = Color(0xFF5F5E5A),
    quoteBar = Color(0xFFB4B2A9),
    quoteText = Color(0xFF5F5E5A),
    rule = Color(0xFFD3D1C7),
    tableBorder = Color(0xFFD3D1C7),
    tableHeaderBg = Color(0xFFE6F1FB),
    tableHeaderText = Color(0xFF0C447C),
    tableCellBg = Color(0xFFFFFFFF),
    codeBg = Color(0xFF1E1E22),
    codeHeaderBg = Color(0xFF2C2C31),
    codeHeaderText = Color(0xFFB4B2A9),
    syntax = CodeSyntax,
    diffHeaderBg = Color(0xFFF1EFE8),
    diffHeaderText = Color(0xFF5F5E5A),
    diffBg = Color(0xFFFFFFFF),
    diffContextText = Color(0xFF444441),
    diffAddedBg = Color(0xFFEAF3DE),
    diffAddedText = Color(0xFF3B6D11),
    diffRemovedBg = Color(0xFFFCEBEB),
    diffRemovedText = Color(0xFFA32D2D),
    diffHunkBg = Color(0xFFE6F1FB),
    diffHunkText = Color(0xFF0C447C),
    note = CalloutColors(Color(0xFFE6F1FB), Color(0xFF378ADD), Color(0xFF0C447C)),
    tip = CalloutColors(Color(0xFFEAF3DE), Color(0xFF639922), Color(0xFF27500A)),
    important = CalloutColors(Color(0xFFEEEDFE), Color(0xFF7F77DD), Color(0xFF3C3489)),
    warning = CalloutColors(Color(0xFFFAEEDA), Color(0xFFBA7517), Color(0xFF633806)),
    caution = CalloutColors(Color(0xFFFCEBEB), Color(0xFFE24B4A), Color(0xFF791F1F)),
)

val DarkMarkdownPalette = MarkdownPalette(
    heading = Color(0xFFF0997B),
    headingBar = Color(0xFFD85A30),
    strong = Color(0xFFFFFFFF),
    inlineCode = Color(0xFFF09595),
    inlineCodeBg = Color(0xFF3D2828),
    link = Color(0xFF85B7EB),
    bullets = listOf(Color(0xFFF0997B), Color(0xFF5DCAA5), Color(0xFFB4B2A9)),
    numbered = Color(0xFFF0997B),
    taskDone = Color(0xFF97C459),
    taskTodo = Color(0xFF888780),
    taskDoneText = Color(0xFF888780),
    quoteBar = Color(0xFF5F5E5A),
    quoteText = Color(0xFFB4B2A9),
    rule = Color(0xFF444441),
    tableBorder = Color(0xFF444441),
    tableHeaderBg = Color(0xFF0C447C),
    tableHeaderText = Color(0xFFE6F1FB),
    tableCellBg = Color(0xFF232326),
    codeBg = Color(0xFF17171A),
    codeHeaderBg = Color(0xFF232327),
    codeHeaderText = Color(0xFFB4B2A9),
    syntax = CodeSyntax,
    diffHeaderBg = Color(0xFF2C2C2A),
    diffHeaderText = Color(0xFFB4B2A9),
    diffBg = Color(0xFF1E1E22),
    diffContextText = Color(0xFFD3D1C7),
    diffAddedBg = Color(0xFF173404),
    diffAddedText = Color(0xFFC0DD97),
    diffRemovedBg = Color(0xFF501313),
    diffRemovedText = Color(0xFFF7C1C1),
    diffHunkBg = Color(0xFF042C53),
    diffHunkText = Color(0xFFB5D4F4),
    note = CalloutColors(Color(0xFF042C53), Color(0xFF378ADD), Color(0xFFB5D4F4)),
    tip = CalloutColors(Color(0xFF173404), Color(0xFF639922), Color(0xFFC0DD97)),
    important = CalloutColors(Color(0xFF26215C), Color(0xFF7F77DD), Color(0xFFCECBF6)),
    warning = CalloutColors(Color(0xFF412402), Color(0xFFBA7517), Color(0xFFFAC775)),
    caution = CalloutColors(Color(0xFF501313), Color(0xFFE24B4A), Color(0xFFF7C1C1)),
)

/** Набір кольорів розмітки для поточного тла. */
@Composable
fun markdownPalette(): MarkdownPalette =
    if (MaterialTheme.colorScheme.surface.luminance() < 0.5f) DarkMarkdownPalette else LightMarkdownPalette
