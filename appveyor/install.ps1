# check that all the required ODBC drivers are available, and install any that are missing

function DownloadFileFromUrl ($url, $file_path) {
    $curl_params = "-f -sS -L -o `"$file_path`" `"$url`""
    if (${env:APVYR_VERBOSE} -eq "true") {
        $curl_params = "-v " + $curl_params
    }
    # try multiple times to download the file
    $attempt_number = 1
    $max_attempts = 5
    while ($attempt_number -le $max_attempts) {
        try {
            Write-Output "Downloading ""$url""..."
            $result = Start-Process curl.exe -ArgumentList $curl_params -NoNewWindow -Wait -PassThru -ErrorAction Stop
            if ($result.ExitCode -eq 0) {
                Write-Output "...downloaded succeeded"
                return
            }
            Write-Output "...download failed with exit code: $($result.ExitCode)"
            # FYI, alternate way to invoke curl using the call operator (&)
            #   & curl.exe -f -sS -L -o $file_path $url
            #   IF ($LASTEXITCODE -eq 0) {return}
        } catch {
            Write-Error $_.Exception.Message
        }
        Write-Output "WARNING: download attempt number $attempt_number of $max_attempts failed"
        Start-Sleep -Seconds 10
        $attempt_number += 1
    }
    # if a downloaded file exists at all it is probably a partial file, so delete it
    if (Test-Path $file_path) {
        Remove-Item $file_path
    }
}

function CheckAndInstallMsiFromUrl ($driver_name, $driver_bitness, $driver_url, $msifile_path, $msiexec_paras) {
    Write-Output ""

    # check whether the driver is already installed
    $d = Get-OdbcDriver -Name $driver_name -Platform $driver_bitness -ErrorAction:SilentlyContinue
    if ($?) {
        Write-Output "*** Driver ""$driver_name"" ($driver_bitness) already installed: $($d.Attribute.Driver)"
        return
    } else {
        Write-Output "*** Driver ""$driver_name"" ($driver_bitness) not found"
    }

    # get the driver's msi file, check the AppVeyor cache first
    if (Test-Path $msifile_path) {
        Write-Output "Driver's msi file found in the cache"
    } else {
        DownloadFileFromUrl -url $driver_url -file_path $msifile_path
        if (-not (Test-Path $msifile_path)) {
            Write-Output "ERROR: Could not download the msi file from ""$driver_url"""
            return
        }
    }

    # install the driver's msi file
    # Note, there is an alternate method of calling msiexec.exe using cmd:
    #   cmd /c start /wait msiexec.exe /i "$msifile_path" /quiet /qn /norestart
    #   if (!$?) {...}
    Write-Output "Installing the driver..."
    $msi_args = @("/quiet", "/passive", "/qn", "/norestart", "/i", ('"{0}"' -f $msifile_path))
    if ($msiexec_paras) {
        $msi_args += $msiexec_paras
    }
    $result = Start-Process "msiexec.exe" -ArgumentList $msi_args -Wait -PassThru
    if ($result.ExitCode -ne 0) {
        Write-Output "ERROR: Driver installation failed"
        Write-Output $result
        # if the msi file can't be installed, delete it
        if (Test-Path $msifile_path) {
            Write-Output "Deleting the msi file from the cache: ""$msifile_path""..."
            Remove-Item $msifile_path
        }
        return
    }
    Write-Output "...driver installed successfully"
}

function CheckAndInstallZippedMsiFromUrl ($driver_name, $driver_bitness, $driver_url, $zipfile_path, $zip_internal_msi_file, $msifile_path) {
    Write-Output ""
    # check whether the driver is already installed
    if ($d = Get-OdbcDriver -Name $driver_name -Platform $driver_bitness -ErrorAction:SilentlyContinue) {
        Write-Output "*** Driver ""$driver_name"" ($driver_bitness) already installed: $($d.Attribute.Driver)"
        return
    } else {
        Write-Output "*** Driver ""$driver_name"" ($driver_bitness) not found"
    }
    if (Test-Path $msifile_path) {
        Write-Output "Driver's msi file found in the cache"
    } else {
        DownloadFileFromUrl -url $driver_url -file_path $zipfile_path
        if (-not (Test-Path $zipfile_path)) {
            Write-Output "ERROR: Could not download the zip file from $driver_url"
            return
        }
        Write-Output "Unzipping..."
        Expand-Archive -Path $zipfile_path -DestinationPath $temp_dir
        Copy-Item -Path "$temp_dir\$zip_internal_msi_file" -Destination $msifile_path -Force
    }
    Write-Output "Installing the driver..."
    $msi_args = @("/i", ('"{0}"' -f $msifile_path), "/quiet", "/qn", "/norestart")
    $result = Start-Process "msiexec.exe" -ArgumentList $msi_args -Wait -PassThru
    if ($result.ExitCode -ne 0) {
        Write-Output "ERROR: Driver installation failed"
        Write-Output $result
        # if the msi file can't be installed, delete it
        if (Test-Path $msifile_path) {
            Write-Output "Deleting the msi file from the cache: ""$msifile_path""..."
            Remove-Item $msifile_path
        }
        return
    }
    Write-Output "...driver installed successfully"
}

function ListOdbcDrivers () {
    Get-OdbcDriver | ForEach-Object {
        $driverFileName = $_.Attribute.Driver
        if (Test-Path $driverFileName) {
            $driverFile = Get-Item $driverFileName
            Write-Output ("MSFT_OdbcDriver (Name = ""{0}"", Platform = ""{1}"", Version = ""{2}"")" -f $_.Name, $_.Platform, $driverFile.VersionInfo.ProductVersion)
        } else {
            Write-Output ("MSFT_OdbcDriver (Name = ""{0}"", Platform = ""{1}"")" -f $_.Name, $_.Platform)
        }
    } | Sort-Object
}

# get Python bitness
$python_arch = cmd /c "${env:PYTHON_HOME}\python" -c "import sys; sys.stdout.write('64' if sys.maxsize > 2**32 else '32')"


# directories used exclusively by AppVeyor
$cache_dir = "$env:APPVEYOR_BUILD_FOLDER\apvyr_cache"
if (Test-Path $cache_dir) {
    Write-Output "*** Contents of the cache directory: $cache_dir"
    Get-ChildItem $cache_dir
} else {
    Write-Output "*** Creating directory ""$cache_dir""..."
    New-Item -ItemType Directory -Path $cache_dir | out-null
}
$temp_dir = "$env:APPVEYOR_BUILD_FOLDER\apvyr_tmp"
if (-not (Test-Path $temp_dir)) {
    Write-Output ""
    Write-Output "*** Creating directory ""$temp_dir""..."
    New-Item -ItemType Directory -Path $temp_dir | out-null
}


# output the already available ODBC drivers before installation
Write-Output ""
Write-Output "*** Installed ODBC drivers:"
ListOdbcDrivers


# Microsoft SQL Server
# AppVeyor build servers are always 64-bit and therefore only the 64-bit
# SQL Server ODBC driver msi files can be installed on them.  However,
# the 64-bit msi files include both 32-bit and 64-bit drivers anyway.

# The "SQL Server Native Client 10.0" and "SQL Server Native Client 11.0" driver
# downloads do not appear to be available, hence cannot be installed.

CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 11 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/5/7/2/57249A3A-19D6-4901-ACCE-80924ABEB267/ENU/x64/msodbcsql.msi" `
    -msifile_path "$cache_dir\msodbcsql_11.0.0.0_x64.msi" `
    -msiexec_paras @("IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL");

# with the 13.0 driver, some tests fail for Python 2.7 so using version 13.1
# 13.0: https://download.microsoft.com/download/1/E/7/1E7B1181-3974-4B29-9A47-CC857B271AA2/English/X64/msodbcsql.msi
# 13.1: https://download.microsoft.com/download/D/5/E/D5EEF288-A277-45C8-855B-8E2CB7E25B96/x64/msodbcsql.msi
CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 13 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/D/5/E/D5EEF288-A277-45C8-855B-8E2CB7E25B96/x64/msodbcsql.msi" `
    -msifile_path "$cache_dir\msodbcsql_13_E25B96_x64.msi" `
    -msiexec_paras @("IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL");

# As of 2026-02-14, https://learn.microsoft.com/en-us/sql/connect/odbc/windows/release-notes-odbc-sql-server-windows:
# 17.0     : https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/17.0.1.1/x64/msodbcsql.msi (2018-02)
# 17.1     : https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/17.1.0.1/x64/msodbcsql.msi
# 17.2     : https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/17.2.0.1/x64/msodbcsql.msi
# 17.3     : https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/17.3.1.1/x64/msodbcsql.msi
# 17.4     : https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/17.4.1.1/x64/msodbcsql.msi
# 17.4.2   : https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/17.4.2.1/x64/msodbcsql.msi
# 17.5     : https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/17.5.1.1/x64/msodbcsql.msi
# 17.5.2   : https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/17.5.2.1/x64/msodbcsql.msi
# 17.6     : https://download.microsoft.com/download/6/b/3/6b3dd05c-678c-4e6b-b503-1d66e16ef23d/en-US/17.6.1.1/x64/msodbcsql.msi
# 17.7     : https://download.microsoft.com/download/2/c/c/2cc12eab-a3aa-45d6-95bb-13f968fb6cd6/en-US/17.7.1.1/x64/msodbcsql.msi
# 17.7.2   : https://download.microsoft.com/download/2/c/c/2cc12eab-a3aa-45d6-95bb-13f968fb6cd6/en-US/17.7.2.1/x64/msodbcsql.msi
# 17.8     : https://download.microsoft.com/download/a/e/b/aeb7d4ff-ca20-45db-86b8-8a8f774ce97b/en-US/17.8.1.1/x64/msodbcsql.msi
# 17.9     : https://download.microsoft.com/download/c/5/f/c5f48103-1c0a-46d6-9e54-def996cd8d76/en-US/17.9.1.1/x64/msodbcsql.msi
# 17.10    : https://download.microsoft.com/download/6/f/f/6ffefc73-39ab-4cc0-bb7c-4093d64c2669/en-US/17.10.1.1/x64/msodbcsql.msi (2022-06-30)
# 17.10.2  : https://download.microsoft.com/download/6/f/f/6ffefc73-39ab-4cc0-bb7c-4093d64c2669/en-US/17.10.2.1/x64/msodbcsql.msi (2022-11-28)
# 17.10.3  : https://download.microsoft.com/download/6/f/f/6ffefc73-39ab-4cc0-bb7c-4093d64c2669/en-US/17.10.3.1/x64/msodbcsql.msi (2023-01-26)
# 17.10.4.1: https://download.microsoft.com/download/6/f/f/6ffefc73-39ab-4cc0-bb7c-4093d64c2669/en-US/17.10.4.1/x64/msodbcsql.msi (2023-06-15)
# 17.10.5  : https://download.microsoft.com/download/6/f/f/6ffefc73-39ab-4cc0-bb7c-4093d64c2669/en-US/17.10.5.1/x64/msodbcsql.msi (2023-10-23)
# 17.10.6  : https://download.microsoft.com/download/6/f/f/6ffefc73-39ab-4cc0-bb7c-4093d64c2669/en-US/17.10.6.1/x64/msodbcsql.msi (2024-04-09)
CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 17 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/6/f/f/6ffefc73-39ab-4cc0-bb7c-4093d64c2669/en-US/17.10.6.1/x64/msodbcsql.msi" `
    -msifile_path "$cache_dir\msodbcsql_17.10.6.1_x64.msi" `
    -msiexec_paras @("IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL");

# As of 2026-02-14, https://learn.microsoft.com/en-us/sql/connect/odbc/windows/release-notes-odbc-sql-server-windows:
# 18.0     : https://download.microsoft.com/download/1/a/4/1a4a49b8-9fe6-4237-be0d-a6b8f2d559b5/en-US/18.0.1.1/x64/msodbcsql.msi (2022-02-15)
# 18.1     : https://download.microsoft.com/download/9/1/f/91fc3f67-34bd-44c7-9431-be5919dc8377/en-US/18.1.1.1/x64/msodbcsql.msi (2022-08-08)
# 18.1.2   : https://download.microsoft.com/download/9/1/f/91fc3f67-34bd-44c7-9431-be5919dc8377/en-US/18.1.2.1/x64/msodbcsql.msi (2022-11-03)
# 18.2     : https://download.microsoft.com/download/c/5/4/c54c2bf1-87d0-4f6f-b837-b78d34d4d28a/en-US/18.2.1.1/x64/msodbcsql.msi (2023-01-31)
# 18.2.2   : https://download.microsoft.com/download/c/5/4/c54c2bf1-87d0-4f6f-b837-b78d34d4d28a/en-US/18.2.2.1/x64/msodbcsql.msi (2023-06-15)
# 18.3.1   : https://download.microsoft.com/download/4/f/e/4fed6f4b-dc42-4255-b4b4-70f8e2a35a63/en-US/18.3.1.1/x64/msodbcsql.msi (2023-07-31)
# 18.3.2   : https://download.microsoft.com/download/4/f/e/4fed6f4b-dc42-4255-b4b4-70f8e2a35a63/en-US/18.3.2.1/x64/msodbcsql.msi (2023-10-10)
# 18.3.3   : https://download.microsoft.com/download/4/f/e/4fed6f4b-dc42-4255-b4b4-70f8e2a35a63/en-US/18.3.3.1/x64/msodbcsql.msi (2024-04-09)
# 18.4     : https://download.microsoft.com/download/1/7/4/17423b83-b75d-42e1-b5b9-eaa266561c5e/Windows/amd64/1033/msodbcsql.msi (2024-07-31)
# 18.5     : https://download.microsoft.com/download/26bc9eb1-ba24-4b62-8274-bff0f935bb75/amd64/1033/msodbcsql.msi (2025-03-17)
# 18.5.2   : https://download.microsoft.com/download/48a8e0c3-556b-4012-ba65-fcea935447f2/amd64/1033/msodbcsql.msi (2025-09-26)
# 18.6     : https://download.microsoft.com/download/8d6e3acc-bf5b-41fe-ad51-a9ad406a780f/amd64/1033/msodbcsql.msi (2025-12-17)
# NOTE 2026-02-14: version 18.6.1.1 causes the MS SQL _test_tvp() unit test to raise an access violation fault, so using 18.5.2 for the time being
CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 18 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/48a8e0c3-556b-4012-ba65-fcea935447f2/amd64/1033/msodbcsql.msi" `
    -msifile_path "$cache_dir\msodbcsql_18.5.2.1_x64.msi" `
    -msiexec_paras @("IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL");

# some drivers must be installed in alignment with Python's bitness
if ($python_arch -eq "64") {

    CheckAndInstallZippedMsiFromUrl `
        -driver_name "PostgreSQL Unicode(x64)" `
        -driver_bitness "64-bit" `
        -driver_url "https://ftp.postgresql.org/pub/odbc/versions.old/msi/psqlodbc_13_02_0000-x64-1.zip" `
        -zipfile_path "$temp_dir\psqlodbc_13_02_0000-x64-1.zip" `
        -zip_internal_msi_file "psqlodbc_x64.msi" `
        -msifile_path "$cache_dir\psqlodbc_13_02_0000-x64.msi";

    CheckAndInstallMsiFromUrl `
        -driver_name "MySQL ODBC 8.4 ANSI Driver" `
        -driver_bitness "64-bit" `
        -driver_url "https://downloads.mysql.com/archives/get/p/10/file/mysql-connector-odbc-8.4.0-winx64.msi" `
        -msifile_path "$cache_dir\mysql-connector-odbc-8.4.0-winx64.msi";

} elseif ($python_arch -eq "32") {

    CheckAndInstallZippedMsiFromUrl `
        -driver_name "PostgreSQL Unicode" `
        -driver_bitness "32-bit" `
        -driver_url "https://ftp.postgresql.org/pub/odbc/versions.old/msi/psqlodbc_13_02_0000-x86-1.zip" `
        -zipfile_path "$temp_dir\psqlodbc_13_02_0000-x86-1.zip" `
        -zip_internal_msi_file "psqlodbc_x86.msi" `
        -msifile_path "$cache_dir\psqlodbc_13_02_0000-x86.msi";

    CheckAndInstallMsiFromUrl `
        -driver_name "MySQL ODBC 8.0 ANSI Driver" `
        -driver_bitness "32-bit" `
        -driver_url "https://dev.mysql.com/get/Downloads/Connector-ODBC/8.0/mysql-connector-odbc-8.0.37-win32.msi" `
        -msifile_path "$cache_dir\mysql-connector-odbc-8.0.37-win32.msi";

} else {
    Write-Output "ERROR: Unexpected Python architecture:"
    Write-Output $python_arch
}


# output the contents of the temporary AppVeyor directories and
# the ODBC drivers now available after installation
Write-Output ""
Write-Output "*** Contents of the cache directory: $cache_dir"
Get-ChildItem $cache_dir
Write-Output ""
Write-Output "*** Contents of the temporary directory: $temp_dir"
Get-ChildItem $temp_dir
Write-Output ""
Write-Output "*** Installed ODBC drivers:"
ListOdbcDrivers
