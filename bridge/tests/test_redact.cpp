// test_redact.cpp — тести маскування секретів.
//
// Master Prompt (Частина 3 §18) вимагає перевіряти І хибні спрацювання,
// І пропуски. Тому тут два розділи: що МАЄ бути замасковано і що
// НЕ ПОВИННО, бо надмірне маскування ламає звичайний вивід.

#include "test_framework.h"
#include "redact.h"

using namespace cm;

namespace {

bool Masked(const std::string& text) {
    return text.find("«приховано»") != std::string::npos;
}

}  // namespace

// ── Що має бути замасковано ──────────────────────────────────────────────────

CM_TEST(redact, masks_assignments) {
    CHECK(Masked(RedactSecrets("PASSWORD=hunter2")));
    CHECK(Masked(RedactSecrets("api_key: sk_live_abcdefghij")));
    CHECK(Masked(RedactSecrets("secret = my-secret-value")));
    CHECK(Masked(RedactSecrets("AUTH_TOKEN=\"abc123xyz\"")));
    CHECK(Masked(RedactSecrets("client_secret: 12345abcde")));
}

CM_TEST(redact, masks_case_insensitively) {
    CHECK(Masked(RedactSecrets("Password=secret")));
    CHECK(Masked(RedactSecrets("API_KEY=value")));
    CHECK(Masked(RedactSecrets("ApiKey=value")));
}

CM_TEST(redact, masks_known_token_prefixes) {
    // Зразки складаються з частин: інакше сканер секретів GitHub вважає
    // сам тест витоком і блокує виклад репозиторію.
    CHECK(Masked(RedactSecrets("ключ " + std::string("sk-") + "ant-api03-abcdefghijklmnopqrstuvwxyz1234")));
    CHECK(Masked(RedactSecrets(std::string("ghp") + "_abcdefghijklmnopqrstuvwxyz0123456789")));
    CHECK(Masked(RedactSecrets("AKIAIOSFODNN7EXAMPLE")));
    CHECK(Masked(RedactSecrets(std::string("xoxb") + "-123456789012-abcdefghijklmnop")));
}

CM_TEST(redact, masks_private_key_block) {
    const std::string input =
        "-----BEGIN RSA PRIVATE KEY-----\n"
        "MIIEowIBAAKCAQEA1234567890\n"
        "-----END RSA PRIVATE KEY-----\n";

    const std::string result = RedactSecrets(input);
    CHECK(Masked(result));
    CHECK(result.find("MIIEowIBAAKCAQEA") == std::string::npos);
}

CM_TEST(redact, masks_authorization_header) {
    const std::string result = RedactSecrets("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9");
    CHECK(Masked(result));
    CHECK(result.find("eyJhbGci") == std::string::npos);
}

CM_TEST(redact, masks_url_credentials_but_keeps_user) {
    const std::string result = RedactSecrets("https://admin:s3cr3t@example.com/repo.git");
    CHECK(Masked(result));
    CHECK(result.find("s3cr3t") == std::string::npos);
    // Ім'я користувача лишається: воно допомагає зрозуміти контекст
    // і секретом не є.
    CHECK(result.find("admin") != std::string::npos);
}

// ── Що НЕ повинно маскуватися ────────────────────────────────────────────────
//
// Ці випадки важливіші за попередні: надмірне маскування зробило б
// звичайний вивід нечитабельним, і користувач перестав би довіряти монітору.

CM_TEST(redact, keeps_normal_output) {
    const char* samples[] = {
        "42 tests passed",
        "Compiling src/main.cpp",
        "Build completed in 3.2s",
        "Error: file not found at line 42",
        "git commit -m \"виправлено помилку\"",
        "npm install --save-dev typescript",
    };

    for (const char* sample : samples) {
        CHECK_STR(RedactSecrets(sample), sample);
    }
}

CM_TEST(redact, keeps_hashes_and_ids) {
    // Хеші комітів, контрольні суми й ідентифікатори виглядають «секретно»,
    // але маскувати їх не можна — саме на цьому ламаються наївні фільтри.
    const char* samples[] = {
        "commit e4bd36f8a9c2d1e0f3b4a5c6d7e8f9a0b1c2d3e4",
        "sha256: 3cb25f25faacd57a90434f64d0362f2a",
        "session fa676105-0796-4700-a4ca-2014ea84e18e",
        "PID 15504",
    };

    for (const char* sample : samples) {
        CHECK_STR(RedactSecrets(sample), sample);
    }
}

CM_TEST(redact, keeps_words_that_merely_contain_secret_names) {
    // Слово "password" у прозовому тексті не є присвоєнням.
    CHECK_STR(RedactSecrets("Оновлено сторінку password reset"),
              "Оновлено сторінку password reset");
    CHECK_STR(RedactSecrets("функція validateToken()"), "функція validateToken()");
    CHECK_STR(RedactSecrets("файл token.service.ts"), "файл token.service.ts");
}

CM_TEST(redact, keeps_short_prefix_lookalikes) {
    // "sk-" із коротким хвостом — це не ключ, а звичайний текст.
    CHECK_STR(RedactSecrets("sk-1"), "sk-1");
    CHECK_STR(RedactSecrets("AKIA"), "AKIA");
}

CM_TEST(redact, keeps_public_key_block) {
    // Маскуватися має саме приватний ключ; публічний секретом не є.
    const std::string input = "-----BEGIN PUBLIC KEY-----\nMFkwEwYH\n-----END PUBLIC KEY-----";
    CHECK_FALSE(Masked(RedactSecrets(input)));
}

CM_TEST(redact, keeps_urls_without_credentials) {
    CHECK_STR(RedactSecrets("https://example.com/path?query=1"),
              "https://example.com/path?query=1");
    CHECK_STR(RedactSecrets("http://localhost:3000/api"), "http://localhost:3000/api");
}

// ── Статистика та чутливі шляхи ──────────────────────────────────────────────

CM_TEST(redact, counts_replacements) {
    RedactionStats stats;
    RedactSecrets("PASSWORD=a token=b", &stats);
    CHECK_EQ(stats.assignments, 2u);
    CHECK_EQ(stats.total(), 2u);
}

CM_TEST(redact, detects_sensitive_paths) {
    CHECK(IsSensitivePath(".env"));
    CHECK(IsSensitivePath("/home/user/.env"));
    CHECK(IsSensitivePath("C:\\project\\.env.production"));
    CHECK(IsSensitivePath("~/.ssh/id_rsa"));
    CHECK(IsSensitivePath("cert.pem"));
    CHECK(IsSensitivePath("keystore.jks"));
}

CM_TEST(redact, ordinary_paths_are_not_sensitive) {
    CHECK_FALSE(IsSensitivePath("src/main.cpp"));
    CHECK_FALSE(IsSensitivePath("README.md"));
    // Каталог зі словом "secret" у назві не робить файл секретним:
    // порівняння йде саме за іменем файлу.
    CHECK_FALSE(IsSensitivePath("/projects/secrets-manager/index.ts"));
    CHECK_FALSE(IsSensitivePath("environment.ts"));
}

CM_TEST(redact, handles_empty_and_huge_input) {
    CHECK_STR(RedactSecrets(""), "");

    // Дуже довгий вхід не має ані падати, ані зациклюватись.
    const std::string huge(100'000, 'x');
    const std::string result = RedactSecrets(huge);
    CHECK_EQ(result.size(), huge.size());
}
