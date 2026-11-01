--vertical_results
SELECT
  full_collation_name AS name,
  collation_tailoring(full_collation_name, 'none') AS none,
  collation_tailoring(full_collation_name, 'ascii_digits') AS digits,
  collation_tailoring(full_collation_name, 'ascii_minus_dot_digits') AS minus_dot_digits,
  collation_tailoring(full_collation_name, 'ascii_alnum') AS alnum,
  collation_tailoring(full_collation_name, 'ascii_ident') AS ident,
  collation_tailoring(full_collation_name, 'ascii') AS ascii,
  '---' AS `---`
FROM INFORMATION_SCHEMA.COLLATION_CHARACTER_SET_APPLICABILITY
WHERE character_set_name=@character_set_name
ORDER BY collation_name LIKE '%_ci', collation_name;
--horizontal_results
