package com.claudemonitor.data

import java.util.concurrent.atomic.AtomicLong

/**
 * Накопичення подій на телефоні.
 *
 * Головне правило: те, що вже прийшло, не зникає. Раніше кожен знімок стану
 * (а Bridge надсилає його раз на 20 секунд) ЗАМІНЮВАВ накопичену історію
 * своїми двадцятьма останніми подіями — і відповіді Claude пропадали
 * просто на очах.
 *
 * Тепер знімок лише доповнює історію тим, чого в ній ще немає: подіями,
 * пропущеними, поки телефон був без зв'язку.
 *
 * Логіка винесена окремо від ViewModel і не залежить від Android — тож
 * перевіряється звичайними тестами.
 */
object EventHistory {

    /**
     * Скільки подій тримається в одній історії.
     *
     * Межа лишається, бо пам'ять телефона не безрозмірна
     * (Частина 4 §21 Master Prompt), але вона на порядки більша за колишні
     * сто подій: день роботи в неї вкладається.
     */
    const val MAX_EVENTS = 2000

    /**
     * Сумарний обсяг тексту в символах. Кількість подій сама по собі
     * пам'ять не обмежує: одна репліка може займати десятки кілобайтів.
     */
    const val MAX_TEXT_CHARS = 1_500_000

    /** Скільки останніх записів переглядати в пошуку попередньої частини. */
    private const val PART_SEARCH_WINDOW = 64

    /** Позначка пропущеної частини — щоб склеєний текст не вводив в оману. */
    private const val GAP_MARKER = "\n…\n"

    private val nextLocalId = AtomicLong(1)

    /**
     * Додає подію в історію — на її місце за часом.
     *
     * Чергову частину довгої репліки дописує до вже отриманих частин тієї самої
     * репліки: користувач бачить одне повідомлення, а не кілька обрізків.
     */
    fun append(
        history: List<Protocol.MonitorEvent>,
        event: Protocol.MonitorEvent,
    ): List<Protocol.MonitorEvent> {
        if (event.partCount > 1 && event.groupId != 0L) {
            val index = findGroup(history, event)
            if (index >= 0) {
                val out = ArrayList(history)
                out[index] = mergePart(history[index], event)
                return trim(out)
            }
        }

        val out = ArrayList<Protocol.MonitorEvent>(history.size + 1)
        out.addAll(history)
        out.add(insertionIndex(history, event), withLocalId(event))
        return trim(out)
    }

    /** Наскільки далеко від кінця шукати місце запису, що прийшов із запізненням. */
    private const val REORDER_WINDOW = 256

    /**
     * Місце нового запису — за часом, а не за порядком надходження.
     *
     * Claude Code записує блок репліки у файл, лише коли його дописано, а
     * повідомлення, надіслане посеред роботи, — одразу. Тож ваше повідомлення
     * приходило раніше за відповідь, яку Claude почав писати ще до нього,
     * і в стрічці вони мінялися місцями.
     *
     * Записи з однаковим часом лишаються в порядку надходження: позначка
     * кінця відповіді має той самий час, що й її останній блок, і стоїть
     * після нього. Запис без часу додається в кінець.
     */
    private fun insertionIndex(
        history: List<Protocol.MonitorEvent>,
        event: Protocol.MonitorEvent,
    ): Int {
        var index = history.size
        if (event.timestampMs <= 0) return index

        val stop = maxOf(0, history.size - REORDER_WINDOW)
        while (index > stop) {
            val previous = history[index - 1]
            if (previous.timestampMs <= 0 || previous.timestampMs <= event.timestampMs) break
            index -= 1
        }
        return index
    }

    /**
     * Доповнює історію подіями зі знімка стану.
     *
     * Знімок містить лише кілька останніх подій кожної задачі. Із нього
     * беруться тільки ті події, яких в історії ще немає.
     *
     * Упізнавання йде за відбитком, а не за номером послідовності: після
     * перезапуску Bridge номери починаються заново, і за ними старі події
     * виглядали б новими — історія б задвоювалась.
     */
    fun mergeSnapshot(
        history: List<Protocol.MonitorEvent>,
        snapshotEvents: List<Protocol.MonitorEvent>,
    ): List<Protocol.MonitorEvent> {
        if (snapshotEvents.isEmpty()) return history

        val known = HashSet<String>(history.size * 2)
        for (event in history) known.add(fingerprint(event))

        var result = history
        for (event in snapshotEvents) {
            if (fingerprint(event) in known) continue
            result = append(result, event)
            // Після першої частини групи наступні вже склеюються з нею,
            // а не додаються окремо: відбиток групи спільний.
            known.add(fingerprint(event))
        }
        return result
    }

    /**
     * Відбиток події — те, що не змінюється між знімками й перезапусками Bridge.
     *
     * Для частин довгого запису відбиток спільний на весь запис: після
     * перезапуску Bridge номер групи вже інший, а склеєний текст із частиною
     * однаково не збігся б.
     */
    fun fingerprint(event: Protocol.MonitorEvent): String = buildString {
        append(event.sessionId).append('|')
        append(event.timestampMs).append('|')
        append(event.kind.wire).append('|')
        if (event.partCount > 1) {
            append("group:").append(event.partCount)
        } else {
            // Ціль — повністю: у знімку вона не скорочується, а шляхи файлів
            // з однієї теки збігаються на перших десятках символів. За коротким
            // префіксом три записи різних файлів злилися б в один.
            append(event.target.orEmpty()).append('|')
            // Текст — лише початок: цього досить, щоб розрізнити записи,
            // а порівнювати кілобайти тексту на кожному знімку марно.
            append(event.text?.take(64).orEmpty())
        }
    }

    /** Обрізає найдавніше, доки історія не вкладеться в обидві межі. */
    fun trim(history: List<Protocol.MonitorEvent>): List<Protocol.MonitorEvent> {
        var totalChars = 0L
        for (event in history) totalChars += textSize(event)

        if (history.size <= MAX_EVENTS && totalChars <= MAX_TEXT_CHARS) return history

        var drop = 0
        var count = history.size
        while (drop < history.size - 1 && (count > MAX_EVENTS || totalChars > MAX_TEXT_CHARS)) {
            totalChars -= textSize(history[drop])
            count -= 1
            drop += 1
        }
        return history.subList(drop, history.size).toList()
    }

    private fun textSize(event: Protocol.MonitorEvent): Int =
        (event.text?.length ?: 0) + (event.target?.length ?: 0)

    private fun findGroup(
        history: List<Protocol.MonitorEvent>,
        event: Protocol.MonitorEvent,
    ): Int {
        val stop = maxOf(0, history.size - PART_SEARCH_WINDOW)
        for (i in history.size - 1 downTo stop) {
            val candidate = history[i]
            if (candidate.groupId == event.groupId &&
                candidate.sessionId == event.sessionId &&
                candidate.kind == event.kind
            ) {
                return i
            }
        }
        return -1
    }

    private fun mergePart(
        existing: Protocol.MonitorEvent,
        part: Protocol.MonitorEvent,
    ): Protocol.MonitorEvent {
        // Повтор уже отриманої частини нічого не додає.
        if (part.part <= existing.part) return existing

        val gap = part.part != existing.part + 1

        // Частинами приходить і текст (репліка, вивід), і ціль дії (довга
        // команда) — склеюємо те поле, яке є.
        fun join(head: String?, tail: String?): String? {
            if (head == null && tail == null) return null
            return buildString {
                append(head.orEmpty())
                if (gap) append(GAP_MARKER)
                append(tail.orEmpty())
            }
        }

        return existing.copy(
            text = join(existing.text, part.text),
            target = join(existing.target, part.target),
            part = part.part,
            lastSequence = maxOf(existing.lastSequence, part.sequence),
        )
    }

    private fun withLocalId(event: Protocol.MonitorEvent): Protocol.MonitorEvent =
        if (event.localId != 0L) event else event.copy(localId = nextLocalId.getAndIncrement())
}
