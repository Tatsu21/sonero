#include <QtTest>

#include <string>

#include "core/Format.h"

// Qualified on purpose: an unqualified call with a std::string argument pulls
// namespace std in through ADL, where <format> may also declare format().
namespace fmt = sonar::fmt;

class FormatTest : public QObject {
    Q_OBJECT

private slots:
    void substitutesInOrder();
    void supportsPositionalFields();
    void supportsFixedPrecision();
    void escapesDoubledBraces();
    void handlesTheFilterChainSyntax();
    void rendersCommonTypes();
    void malformedInputIsCopiedThrough();
};

void FormatTest::substitutesInOrder() {
    QCOMPARE(fmt::format("a{}b{}c", 1, 2), std::string("a1b2c"));
    QCOMPARE(fmt::format("{}", std::string("x")), std::string("x"));
    QCOMPARE(fmt::format("no fields"), std::string("no fields"));
}

void FormatTest::supportsPositionalFields() {
    // The filter-chain builder reuses one argument several times.
    QCOMPARE(fmt::format("{0}-{1}-{0}", "a", "b"), std::string("a-b-a"));
    QCOMPARE(fmt::format("{2}{0}", "x", "y", "z"), std::string("zx"));
}

void FormatTest::supportsFixedPrecision() {
    QCOMPARE(fmt::format("{:.2f}", 3.14159), std::string("3.14"));
    QCOMPARE(fmt::format("{:.4f}", 1.0), std::string("1.0000"));
    QCOMPARE(fmt::format("{:.0f}", 2.5), std::string("2"));  // banker's rounding, as iostreams do
    QCOMPARE(fmt::format("{:.1f} {:.3f}", 1.25, 2.0), std::string("1.2 2.000"));
}

void FormatTest::escapesDoubledBraces() {
    QCOMPARE(fmt::format("{{}}"), std::string("{}"));
    QCOMPARE(fmt::format("{{ {} }}", 7), std::string("{ 7 }"));
    QCOMPARE(fmt::format("{{{}}}", 1), std::string("{1}"));
}

void FormatTest::handlesTheFilterChainSyntax() {
    // A shape lifted straight from the PipeWire module arguments, which mix
    // literal braces, positional fields and precision in one string.
    const std::string out =
        fmt::format(R"({{ name = "{}_eq{}" control = {{ "Freq" = {:.4f} }} }})", "sonero_game", 3,
               1000.0);
    QCOMPARE(out, std::string(R"({ name = "sonero_game_eq3" control = { "Freq" = 1000.0000 } })"));
}

void FormatTest::rendersCommonTypes() {
    QCOMPARE(fmt::format("{}", 42), std::string("42"));
    QCOMPARE(fmt::format("{}", -1), std::string("-1"));
    QCOMPARE(fmt::format("{}", 'c'), std::string("c"));
    QCOMPARE(fmt::format("{}", "literal"), std::string("literal"));
    // bool must read as a word, not as 1/0.
    QCOMPARE(fmt::format("{}", true), std::string("true"));
    QCOMPARE(fmt::format("{}", false), std::string("false"));
}

void FormatTest::malformedInputIsCopiedThrough() {
    // Logging must never throw or truncate just because a format string is wrong.
    QCOMPARE(fmt::format("{"), std::string("{"));
    QCOMPARE(fmt::format("unterminated {0", 1), std::string("unterminated {0"));
    QCOMPARE(fmt::format("{} and {}", 1), std::string("1 and {}"));   // too few arguments
    QCOMPARE(fmt::format("{:x}", 255), std::string("{:x}"));          // unsupported spec
    QCOMPARE(fmt::format("{}", 1, 2), std::string("1"));              // extra arguments ignored
}

QTEST_GUILESS_MAIN(FormatTest)
#include "FormatTest.moc"
