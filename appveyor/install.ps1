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
Get-OdbcDriver | ForEach-Object -Process {Write-Output "$_"} | Sort-Object


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

CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 17 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/6/f/f/6ffefc73-39ab-4cc0-bb7c-4093d64c2669/en-US/17.10.6.1/x64/msodbcsql.msi" `
    -msifile_path "$cache_dir\msodbcsql_17.10.6.1_x64.msi" `
    -msiexec_paras @("IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL");

CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 18 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/1/7/4/17423b83-b75d-42e1-b5b9-eaa266561c5e/Windows/amd64/1033/msodbcsql.msi" `
    -msifile_path "$cache_dir\msodbcsql_18_561c5e_x64.msi" `
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
Get-OdbcDriver | ForEach-Object -Process {Write-Output "$_"} | Sort-Object
