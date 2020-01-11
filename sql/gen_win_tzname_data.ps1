# Generates a header file for converting between Windows timezone names to tzdb names
# using CLDR data.
# Usage: powershell -File gen_win_tzname_data.ps1 >  win_tzname_data.h

write-output  "/* This file  was generated using gen_win_tzname_data.ps1 */"
$xdoc = new-object System.Xml.XmlDocument
$xdoc.load("https://unicode.org/repos/cldr/trunk/common/supplemental/windowsZones.xml")
$nodes = $xdoc.SelectNodes("//mapZone[@territory='001']") # use default territory (001)
foreach ($node in $nodes) {
  write-output ('{L"'+ $node.other + '","'+ $node.type+'"},')
}
