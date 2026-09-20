// json.h — мінімальний JSON для Bridge.
//
// Чому власна реалізація, а не бібліотека: Bridge потребує рівно двох
// можливостей — розібрати рядок транскрипту та зібрати кадр протоколу.
// Повноцінна бібліотека JSON коштувала б сотні кілобайтів коду і зайвих
// залежностей заради функцій, які тут не потрібні
// (Частина 4 §30, Частина 6 §39 Master Prompt).
//
// Парсер написаний з припущенням, що вхідні дані ВОРОЖІ: він не рекурсує
// без обмежень, не виділяє пам'ять без меж і не кидає винятків.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cm::json {

/// Обмеження глибини вкладеності. Захист від переповнення стека на
/// зловмисно глибокому документі на кшталт "[[[[[[...".
inline constexpr int kMaxDepth = 32;

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

class Value;
using Array  = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

class Value {
public:
    Value() = default;
    explicit Value(bool v)          : type_(Type::Bool), bool_(v) {}
    explicit Value(double v)        : type_(Type::Number), number_(v) {}
    explicit Value(std::string v)   : type_(Type::String), string_(std::move(v)) {}
    explicit Value(Array v)         : type_(Type::Array), array_(std::move(v)) {}
    explicit Value(Object v)        : type_(Type::Object), object_(std::move(v)) {}

    Type type() const { return type_; }

    bool isNull()   const { return type_ == Type::Null; }
    bool isBool()   const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool        asBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
    double      asNumber(double fallback = 0.0) const { return isNumber() ? number_ : fallback; }
    int64_t     asInt(int64_t fallback = 0) const;
    const std::string& asString() const;
    std::string asStringOr(std::string_view fallback) const;

    const Array&  asArray()  const;
    const Object& asObject() const;

    /// Пошук поля обʼєкта. Повертає null-значення, якщо поля немає —
    /// це дозволяє ланцюжкові звернення без перевірок на кожному кроці.
    const Value& operator[](std::string_view key) const;

    /// Елемент масиву за індексом; поза межами — null-значення.
    const Value& operator[](size_t index) const;

    /// Пошук за шляхом виду "message.content" — зручно для транскриптів.
    const Value& path(std::string_view dottedPath) const;

    size_t size() const;
    bool   has(std::string_view key) const;

private:
    Type        type_ = Type::Null;
    bool        bool_ = false;
    double      number_ = 0.0;
    std::string string_;
    Array       array_;
    Object      object_;
};

/// Розбирає документ JSON.
/// @returns true, якщо документ коректний і повністю розібраний.
bool Parse(std::string_view text, Value& out);

// ── Генерація ────────────────────────────────────────────────────────────────
//
// Збирання кадрів зроблено потоковим: результат одразу пишеться в рядок,
// без побудови проміжного дерева. Це прибирає зайві виділення пам'яті
// на найгарячішому шляху — відправленні подій.

class Writer {
public:
    explicit Writer(std::string& out) : out_(out) {}

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();

    void key(std::string_view name);

    void valueNull();
    void value(bool v);
    void value(int64_t v);
    void value(uint64_t v);
    void value(double v);
    void value(std::string_view v);
    void value(const char* v) { value(std::string_view(v)); }

    /// Вставляє вже готовий фрагмент JSON без екранування.
    /// Викликати лише з даними, які згенерував цей самий Writer.
    void raw(std::string_view fragment);

    // Скорочення для найчастішого випадку — поле з простим значенням.
    //
    // Перевантаження для const char* обов'язкове. Без нього рядковий
    // літерал перетворювався б на bool: перетворення покажчика на bool
    // є стандартним, а на string_view — користувацьким, тож компілятор
    // мовчки обирав би перше й писав у JSON true замість тексту.
    void field(std::string_view name, const char* v)      { key(name); value(std::string_view(v)); }
    void field(std::string_view name, std::string_view v) { key(name); value(v); }
    void field(std::string_view name, const std::string& v) { key(name); value(std::string_view(v)); }
    void field(std::string_view name, int64_t v)          { key(name); value(v); }
    void field(std::string_view name, uint64_t v)         { key(name); value(v); }
    void field(std::string_view name, double v)           { key(name); value(v); }
    void field(std::string_view name, bool v)             { key(name); value(v); }

private:
    void separator();

    std::string& out_;
    bool needComma_ = false;
    bool afterKey_  = false;
};

/// Екранує рядок за правилами JSON, включно з керівними символами.
void EscapeString(std::string_view text, std::string& out);

}  // namespace cm::json
