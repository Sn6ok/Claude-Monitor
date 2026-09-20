package com.claudemonitor

import com.claudemonitor.ui.MdBlock
import com.claudemonitor.ui.parseMarkdown
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Тести розбору розмітки.
 *
 * Розбір працює над текстом, який Claude пише вільно, а Bridge ще й обрізає
 * за розміром. Тому перевіряється не лише правильний випадок, а й обірваний
 * та навмисно поламаний вхід: розбір не має ані падати, ані ковтати текст.
 */
class MarkdownTest {

    private val fence = "```"

    @Test
    fun `звичайний текст лишається абзацом`() {
        val blocks = parseMarkdown("просто рядок тексту")
        assertEquals(1, blocks.size)
        assertEquals("просто рядок тексту", (blocks[0] as MdBlock.Paragraph).text)
    }

    @Test
    fun `блок коду відокремлюється від тексту`() {
        val source = "Ось приклад:\n${fence}kotlin\nval a = 1\nval b = 2\n$fence\nготово"
        val blocks = parseMarkdown(source)

        assertEquals(3, blocks.size)
        assertTrue(blocks[0] is MdBlock.Paragraph)
        val code = blocks[1] as MdBlock.Code
        assertEquals("kotlin", code.language)
        assertEquals("val a = 1\nval b = 2", code.code)
        assertEquals("готово", (blocks[2] as MdBlock.Paragraph).text)
    }

    @Test
    fun `обірваний блок коду не ковтає решту тексту мовчки`() {
        // Bridge обрізає текст за розміром події, тож закривальної огорожі
        // може не бути. Текст усе одно має потрапити в блок коду, а не зникнути.
        val blocks = parseMarkdown("${fence}bash\nnpm run build\nnpm test")
        assertEquals(1, blocks.size)
        val code = blocks[0] as MdBlock.Code
        assertEquals("npm run build\nnpm test", code.code)
    }

    @Test
    fun `списки розпізнаються за маркером`() {
        val blocks = parseMarkdown("- перший\n- другий\n1. крок\n2) інший крок")

        assertEquals("перший", (blocks[0] as MdBlock.Bullet).text)
        assertEquals("другий", (blocks[1] as MdBlock.Bullet).text)
        val first = blocks[2] as MdBlock.Numbered
        assertEquals("1.", first.marker)
        assertEquals("крок", first.text)
        assertEquals("2)", (blocks[3] as MdBlock.Numbered).marker)
    }

    @Test
    fun `заголовок потребує пробілу після решітки`() {
        val blocks = parseMarkdown("## Заголовок\n#Не заголовок")

        val heading = blocks[0] as MdBlock.Heading
        assertEquals(2, heading.level)
        assertEquals("Заголовок", heading.text)
        // Без пробілу це звичайний текст: «#1» у переліку не має ставати назвою.
        assertTrue(blocks[1] is MdBlock.Paragraph)
    }

    @Test
    fun `рядок з дефісів є лінією а не пунктом списку`() {
        val blocks = parseMarkdown("---")
        assertEquals(listOf(MdBlock.Rule), blocks)
    }

    @Test
    fun `цитата втрачає службовий символ`() {
        val blocks = parseMarkdown("> важлива примітка")
        assertEquals("важлива примітка", (blocks[0] as MdBlock.Quote).text)
    }

    @Test
    fun `порожній текст не дає жодного блоку`() {
        assertEquals(0, parseMarkdown("").size)
        assertEquals(0, parseMarkdown("\n\n\n").size)
    }

    @Test
    fun `розбір витримує текст із самих службових символів`() {
        // Такий текст не мусить розбиратися «правильно» — він мусить
        // не завісити застосунок і не втратити символи.
        val source = "*".repeat(500) + "\n" + "`".repeat(300) + "\n[[[(((".repeat(50)
        val blocks = parseMarkdown(source)
        assertTrue(blocks.isNotEmpty())
    }

    @Test
    fun `рядки абзацу не злипаються в одну стрічку`() {
        val blocks = parseMarkdown("Перевірок: 2947\nПройдено: 110\nПровалено: 0")
        assertEquals(1, blocks.size)
        assertEquals(
            "Перевірок: 2947\nПройдено: 110\nПровалено: 0",
            (blocks[0] as MdBlock.Paragraph).text,
        )
    }

    @Test
    fun `відступ на початку рядка зберігається`() {
        // У виводі команд відступом вирівняні колонки.
        val blocks = parseMarkdown("  OK    перший\n  OK    другий")
        assertEquals("  OK    перший\n  OK    другий", (blocks[0] as MdBlock.Paragraph).text)
    }

    @Test
    fun `текст без розмітки зберігається символ у символ`() {
        val source = "шлях D:\\Wall\\For Hacker\\file.cpp і 5 * 3 = 15"
        val blocks = parseMarkdown(source)
        assertEquals(source, (blocks[0] as MdBlock.Paragraph).text)
    }

    // ── Таблиці ──────────────────────────────────────────────────────────────

    @Test
    fun `таблиця розбирається на шапку вирівнювання й рядки`() {
        val blocks = parseMarkdown("| Тест | Результат |\n|:-----|----------:|\n| hook | 44/44 |\n| ui | 12/12 |")

        val table = blocks.single() as MdBlock.Table
        assertEquals(listOf("Тест", "Результат"), table.header)
        assertEquals(
            listOf(com.claudemonitor.ui.TableAlign.START, com.claudemonitor.ui.TableAlign.END),
            table.align,
        )
        assertEquals(listOf(listOf("hook", "44/44"), listOf("ui", "12/12")), table.rows)
    }

    @Test
    fun `неповний рядок таблиці доповнюється порожніми клітинками`() {
        val table = parseMarkdown("| a | b |\n|---|---|\n| 1 |").single() as MdBlock.Table
        assertEquals(listOf(listOf("1", "")), table.rows)
    }

    @Test
    fun `риски без рядка-роздільника лишаються текстом`() {
        assertTrue(parseMarkdown("| це | не таблиця |").single() is MdBlock.Paragraph)
    }

    @Test
    fun `екранована риска лишається всередині клітинки`() {
        val table = parseMarkdown("| a \\| b | c |\n|---|---|").single() as MdBlock.Table
        assertEquals(listOf("a | b", "c"), table.header)
    }

    // ── Чекбокси ─────────────────────────────────────────────────────────────

    @Test
    fun `пункти з квадратними дужками стають чекбоксами`() {
        val blocks = parseMarkdown("- [x] тести пройдено\n- [ ] ручна перевірка")

        assertEquals(MdBlock.Task(0, true, "тести пройдено"), blocks[0])
        assertEquals(MdBlock.Task(0, false, "ручна перевірка"), blocks[1])
    }

    @Test
    fun `посилання на початку пункту не є чекбоксом`() {
        assertTrue(parseMarkdown("- [документація](https://example.com)").single() is MdBlock.Bullet)
    }

    // ── Цитати й виноски ─────────────────────────────────────────────────────

    @Test
    fun `суміжні рядки цитати складають один блок`() {
        val quote = parseMarkdown("> перший\n> другий").single() as MdBlock.Quote
        assertEquals("перший\nдругий", quote.text)
    }

    @Test
    fun `виноска з типом на окремому рядку`() {
        val callout = parseMarkdown("> [!WARNING]\n> Перезапустіть Bridge").single() as MdBlock.Callout
        assertEquals(com.claudemonitor.ui.CalloutKind.WARNING, callout.kind)
        assertEquals("Перезапустіть Bridge", callout.text)
    }

    @Test
    fun `виноска в один рядок`() {
        val callout = parseMarkdown("> [!NOTE] коротко").single() as MdBlock.Callout
        assertEquals(com.claudemonitor.ui.CalloutKind.NOTE, callout.kind)
        assertEquals("коротко", callout.text)
    }

    @Test
    fun `невідомий тип виноски лишається звичайною цитатою`() {
        // Вигадувати колір для незнайомого типу не можна — показуємо як є.
        val quote = parseMarkdown("> [!FOO] текст").single() as MdBlock.Quote
        assertEquals("[!FOO] текст", quote.text)
    }
}
