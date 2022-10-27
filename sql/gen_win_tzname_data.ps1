# Generates a header file for converting between Windows timezone names to tzdb names
# using CLDR data.
# Usage: powershell -File gen_win_tzname_data.ps1 >  win_tzname_data.h

write-output  "/* This file  was generated using gen_win_tzname_data.ps1 */"
$xdoc = new-object System.Xml.XmlDocument
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$xdoc.load("https://raw.githubusercontent.com/unicode-org/cldr/master/common/supplemental/windowsZones.xml")
$nodes = $xdoc.SelectNodes("//mapZone[@territory='001']") # use default territory (001)
foreach ($node in $nodes) {
  write-output ('{L"'+ $node.other + '","'+ $node.type+'"},')
}
