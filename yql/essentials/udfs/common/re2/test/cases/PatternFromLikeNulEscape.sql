-- Call PatternFromLike as a UDF, not as a Re2 matcher factory/run config.
$convert = Udf(Re2::PatternFromLike);
$nul = '\x00's;
$percent = Re2::Match($convert('\x00%'s, $nul));
$underscore = Re2::Match($convert('\x00_'s, $nul));
$doubled = Re2::Match($convert('\x00\x00's, $nul));
$doubledPercent = Re2::Match($convert('\x00\x00%'s, $nul));
$doubledUnderscore = Re2::Match($convert('\x00\x00_'s, $nul));
$slashPercent = Re2::Match($convert('\x00\\%'s, $nul));
$slashUnderscore = Re2::Match($convert('\x00\\_'s, $nul));
$meta = Re2::Match($convert('\x00.\x00[\x00]'s, $nul));
$ordinary = Re2::Match($convert('a\x00b's, $nul));
$nulDigit = Re2::Match($convert('\x00\x001's, $nul));
$plainNul = Re2::Match($convert('a\x00b's));
$hashNul = Re2::Match($convert('a\x00b's, '#'s));
$hashPercent = Re2::Match($convert('#%'s, '#'s));
$hashSlashPercent = Re2::Match($convert(@@#\%@@, '#'s));
$hashSlashUnderscore = Re2::Match($convert(@@#\_@@, '#'s));

SELECT
    $percent('%'s) AND NOT $percent('\x00x's) AND NOT $percent('x's) AS EscapedPercent,
    $underscore('_'s) AND NOT $underscore('\x00x's) AND NOT $underscore('x's) AS EscapedUnderscore,
    $doubled($nul) AND NOT $doubled(''s) AND NOT $doubled('\x00\x00's) AS DoubledEscape,
    $doubledPercent($nul) AND $doubledPercent('\x00x\ny's) AND NOT $doubledPercent('x's) AS WildcardAfterDoubledEscape,
    $doubledUnderscore('\x00x's) AND NOT $doubledUnderscore($nul) AND NOT $doubledUnderscore('\x00xy's) AS SingleAfterDoubledEscape,
    $slashPercent('\\'s) AND $slashPercent('\\x's) AND NOT $slashPercent('x's) AS PercentAfterEscapedBackslash,
    $slashUnderscore('\\x's) AND NOT $slashUnderscore('\\'s) AND NOT $slashUnderscore('x's) AS UnderscoreAfterEscapedBackslash,
    $meta('.[]'s) AND NOT $meta('abc's) AS EscapedRegexMetacharacters,
    $ordinary('ab's) AND NOT $ordinary('a\x00b's) AS EscapedOrdinaryCharacter,
    $nulDigit('\x001's) AND NOT $nulDigit('\x01's) AS LiteralNulBeforeDigit,
    $plainNul('a\x00b's) AND NOT $plainNul('ab's) AS NulWithoutEscape,
    $hashNul('a\x00b's) AND NOT $hashNul('ab's) AS NulWithHashEscape,
    $hashPercent('%'s) AND NOT $hashPercent('xx's) AS HashEscapeControl,
    $hashSlashPercent('\\'s) AND $hashSlashPercent('\\x's) AND NOT $hashSlashPercent('x's) AS HashBackslashPercentControl,
    $hashSlashUnderscore('\\x's) AND NOT $hashSlashUnderscore('\\'s) AS HashBackslashUnderscoreControl,
    $convert(''s, $nul) == '(?s)'s AS EmptyPattern,
    -- Compatibility only: literal SQL rejects trailing escapes before this UDF.
    -- Preserve the pre-existing direct-UDF result until its error contract is changed separately.
    $convert('a\x00's, $nul) == $convert('a\x00's) AS TrailingEscapeCompatibility,
    $convert('\x00%\x00's, $nul) == $convert('\x00%\x00's) AS TrailingEscapeAfterWildcardCompatibility;
