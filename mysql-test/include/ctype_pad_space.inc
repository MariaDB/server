SELECT strcmp('a','a '),   strcmp('a ','a');
SELECT strcmp('a\0','a' ), strcmp('a','a\0');
SELECT strcmp('a\0','a '), strcmp('a ','a\0');
SELECT strcmp('a\t','a' ), strcmp('a', 'a\t');
SELECT strcmp('a\t','a '), strcmp('a ', 'a\t');
