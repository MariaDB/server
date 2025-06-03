$assembly = [system.reflection.Assembly]::LoadWithPartialName("MySql.Data")
if ($assembly -eq $null)
{
  "Can't load assembly MySql.Data"
  exit 100
}

try
{
  $connectionString =[string]::Format("server=127.0.0.1;uid=root;port={0};Connection Reset=true;",$Env:MASTER_MYPORT)
  $connection = [MySql.Data.MySqlClient.MySqlConnection]@{ConnectionString=$connectionString}
  $connection.Open()

  # Test ExecuteReader()
  $command = New-Object MySql.Data.MySqlClient.MySqlCommand
  $command.Connection = $connection
  $command.CommandText = "SELECT @@old_mode"
  $reader = $command.ExecuteReader()
  $reader.GetName(0)
  while ($reader.Read())
  {
    $reader.GetValue(0)
  }

  # Test connection reset
  $connection.Close()
  $connection.Open()
  # Test ExecuteNonQuery()
  $command.CommandText="do 1";
  $affected_rows = $command.ExecuteNonQuery()
  if ($affected_rows -ne 0)
  {
    "Expected affected rows 0, actual $affected_rows"
    exit 1
  }
  # Test Prepared Statement
  $command.CommandText = "SELECT @var";
  [void]$command.Parameters.AddWithValue("@var", 1);
  $command.Prepare();
  $out = $command.ExecuteScalar();
  if ($out -ne 1)
  {
    "Expected output 1, actual $out"
    exit 1
  }
  $connection.Close()
}
catch
{
  # Dump exception
  $_
  $inner = $PSItem.Exception.InnerException
  if ($inner -ne  $null)
  {
    $PSItem.Exception.InnerException.Message
    $PSItem.Exception.InnerException.StackTrace
  }
}
