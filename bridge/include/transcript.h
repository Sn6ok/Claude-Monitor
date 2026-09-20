// transcript.h — читання транскриптів Claude Code та нормалізація подій.
//
// Транскрипт (`~/.claude/projects/<slug>/<sessionId>.jsonl`) — це append-only
// файл, який Claude Code веде у будь-якому разі, незалежно від існування
// Bridge. Читання цього файлу не впливає на роботу Claude Code жодним чином:
// файл відкривається з повним набором прапорців спільного доступу, а читання
// завжди інкрементальне — з останнього зміщення.
//
// Формат перевірено емпірично, див. docs/research.md §3.2.

#pragma once

#include "events.h"
#include "json.h"

#include <functional>
#include <vector>

namespace cm {

/// Перетворює назву інструмента Claude Code на нормалізовану дію.
/// Невідома назва дає ActivityAction::Other — вигадувати нову категорію
/// заборонено (Частина 5 §12 Master Prompt).
ActivityAction MapToolToAction(std::string_view toolName);

/// Витягує короткий опис цілі дії з аргументів інструмента.
/// Повертає шлях до файлу, команду або шаблон пошуку — і нічого більше.
/// Повний вміст аргументів на телефон не передається ніколи.
std::string ExtractActionTarget(std::string_view toolName, const json::Value& input);

/// Обробляє один рядок транскрипту, оновлює стан сесії та породжує події.
/// @returns true, якщо рядок розпізнано (навіть якщо подій не породжено)
bool ProcessTranscriptLine(std::string_view line, SessionState& session, const EventSink& sink);

/// Оголошує притриманий кінець відповіді (див. SessionState::heldTurnEnd).
/// Викликається, коли репліку дописано: нових рядків у файлі немає.
void FlushHeldTurnEnd(SessionState& session, const EventSink& sink);

/// Інкрементальний читач одного файлу транскрипту.
class TranscriptReader {
public:
    /// Дочитує файл із поточного зміщення сесії та породжує події.
    /// @returns кількість оброблених рядків
    static size_t ReadNew(SessionState& session, const EventSink& sink);

    /// Зчитує «хвіст» файлу при першому підключенні до сесії, що вже триває.
    /// Обробляються лише останні рядки: повна історія не потрібна й лише
    /// марнувала б пам'ять (Частина 4 §6 Master Prompt).
    ///
    /// Перед хвостом читається невеликий фрагмент початку файлу — виключно
    /// заради метаданих задачі (назва, робочий каталог, гілка). Без цього
    /// назвою стала б випадкова репліка з середини розмови замість опису
    /// початкової задачі. Події з голови файлу НЕ породжуються.
    static size_t ReadTail(SessionState& session, size_t maxLines, const EventSink& sink);

    /// Читає початок файлу лише для метаданих. Події ігноруються.
    static void ReadHeadMetadata(SessionState& session);

private:
    /// Розділяє буфер на завершені рядки. Незавершений хвіст лишається
    /// в буфері до наступного читання: JSONL дописується порціями,
    /// і рядок цілком може прийти розірваним на два записи.
    static void SplitLines(std::string_view buffer,
                           std::vector<std::string_view>& lines,
                           size_t& consumedBytes);
};

}  // namespace cm
