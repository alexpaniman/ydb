-- Call the UDF directly: this cannot be replaced by SQL's literal-LIKE rewrite.
SELECT
    Udf(Re2::PatternFromLike)(@@#\%@@, '#') == @@(?s)\\.*@@ AS PercentAfterEscapedBackslash,
    Udf(Re2::PatternFromLike)(@@#\_@@, '#') == @@(?s)\\.@@ AS UnderscoreAfterEscapedBackslash,
    Udf(Re2::PatternFromLike)('#%', '#') == @@(?s)\%@@ AS EscapedPercentControl;
