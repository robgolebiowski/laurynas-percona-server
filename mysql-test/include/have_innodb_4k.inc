--disable_warnings
if (`SELECT COUNT(*) = 0 FROM performance_schema.global_status WHERE LOWER(variable_name) = 'innodb_page_size' AND variable_value = 4096`)
{
  --skip Test requires InnoDB with 4k Page size.
}
--enable_warnings
