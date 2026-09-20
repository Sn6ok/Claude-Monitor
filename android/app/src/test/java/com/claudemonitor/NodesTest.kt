package com.claudemonitor

import com.claudemonitor.data.Protocol
import com.claudemonitor.i18n.EnStrings
import com.claudemonitor.i18n.UkStrings
import com.claudemonitor.ui.nodeErrorText
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Тести вузлів — розширень, які власник ноутбука кладе в папку `nodes`.
 *
 * Головне тут: застосунок показує рівно те, що надіслав вузол, і чесно
 * каже, коли даних немає.
 */
class NodesTest {

    private fun snapshotWith(nodesJson: String) = Protocol.parseSnapshot(
        """{"t":"snapshot","seq":7,"bridge":{"host":"pc"},"sessions":[],"nodes":$nodesJson}"""
    )

    @Test
    fun `картка вузла читається зі знімка`() {
        val snapshot = snapshotWith(
            """[{"id":"system-info","name":"Система","status":"ok","updated_at":123,
               "lines":[{"label":"CPU","value":"12%"},{"label":"Диск","value":"240 ГБ"}],
               "text":"усе гаразд"}]"""
        )

        assertNotNull(snapshot)
        val node = snapshot!!.nodes.single()
        assertEquals("system-info", node.id)
        assertEquals("Система", node.name)
        assertEquals("ok", node.status)
        assertEquals(2, node.lines.size)
        assertEquals("CPU", node.lines[0].label)
        assertEquals("240 ГБ", node.lines[1].value)
        assertEquals("усе гаразд", node.text)
        assertEquals(Protocol.NodeError.NONE, node.error)
        assertEquals(123L, node.updatedAtMs)
    }

    @Test
    fun `причина відмови приходить кодом, а не чужим текстом`() {
        val node = snapshotWith("""[{"id":"gpu","name":"GPU","status":"error","error":"timeout"}]""")!!
            .nodes.single()

        assertEquals(Protocol.NodeError.TIMEOUT, node.error)
        assertTrue(node.lines.isEmpty())
        assertNull(node.text)

        // Пояснення — мовою застосунку.
        assertEquals("вузол не встиг відповісти", nodeErrorText(node.error, UkStrings))
        assertEquals("the node did not answer in time", nodeErrorText(node.error, EnStrings))
    }

    @Test
    fun `невідомий код відмови не вигадується`() {
        val node = snapshotWith("""[{"id":"x","status":"error","error":"щось нове"}]""")!!.nodes.single()
        assertEquals(Protocol.NodeError.NONE, node.error)
        assertEquals("", nodeErrorText(node.error, UkStrings))
    }

    @Test
    fun `знімок без вузлів лишається без вузлів`() {
        val snapshot = Protocol.parseSnapshot("""{"t":"snapshot","seq":1,"sessions":[]}""")
        assertNotNull(snapshot)
        assertTrue(snapshot!!.nodes.isEmpty())
    }

    @Test
    fun `вузол без ідентифікатора відкидається`() {
        assertTrue(snapshotWith("""[{"name":"Без id","status":"ok"}]""")!!.nodes.isEmpty())
    }

    @Test
    fun `назва береться з ідентифікатора, якщо вузол її не дав`() {
        val node = snapshotWith("""[{"id":"disk","status":"ok","lines":[{"label":"C","value":"50 ГБ"}]}]""")!!
            .nodes.single()
        assertEquals("disk", node.name)
    }

    @Test
    fun `кількість вузлів і рядків обмежена`() {
        val many = (1..40).joinToString(",") { """{"id":"n$it","name":"n","status":"ok"}""" }
        assertEquals(Protocol.MAX_NODES, snapshotWith("[$many]")!!.nodes.size)

        val lines = (1..30).joinToString(",") { """{"label":"l$it","value":"v"}""" }
        val node = snapshotWith("""[{"id":"many","status":"ok","lines":[$lines]}]""")!!.nodes.single()
        assertEquals(Protocol.MAX_NODE_LINES, node.lines.size)
    }
}
