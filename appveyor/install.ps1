# check that all the required ODBC drivers are available, and install them if they are missing

Function CheckAndInstallMsiFromUrl ($driver_name, $driver_bitness, $driver_url, $msifile_path, $msiexec_paras) {
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
        Start-FileDownload -Url $driver_url -FileName $msifile_path
        if (!$?) {
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
        return

    }
    Write-Output "...driver installed successfully"
}

Function CheckAndInstallZippedMsiFromUrl ($driver_name, $driver_bitness, $driver_url, $zipfile_path, $zip_internal_msi_file, $msifile_path) {
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
        Start-FileDownload -Url $driver_url -FileName $zipfile_path
        if (!$?) {
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
        return
    }
    Write-Output "...driver installed successfully"
}


# get python version and bitness
$python_major_version = cmd /c "${env:PYTHON_HOME}\python" -c "import sys; sys.stdout.write(str(sys.version_info.major))"
$python_minor_version = cmd /c "${env:PYTHON_HOME}\python" -c "import sys; sys.stdout.write(str(sys.version_info.minor))"
$python_arch = cmd /c "${env:PYTHON_HOME}\python" -c "import sys; sys.stdout.write('64' if sys.maxsize > 2**32 else '32')"


# directories used exclusively by AppVeyor
$cache_dir = "$env:APPVEYOR_BUILD_FOLDER\apvyr_cache"
If (Test-Path $cache_dir) {
    Write-Output "*** Contents of the cache directory: $cache_dir"
    Get-ChildItem $cache_dir
} else {
    Write-Output "*** Creating directory ""$cache_dir""..."
    New-Item -ItemType Directory -Path $cache_dir | out-null
}
$temp_dir = "$env:APPVEYOR_BUILD_FOLDER\apvyr_tmp"
If (-Not (Test-Path $temp_dir)) {
    Write-Output "*** Creating directory ""$temp_dir""..."
    New-Item -ItemType Directory -Path $temp_dir | out-null
}


If (${env:APVYR_VERBOSE} -eq "true") {
    Write-Output ""
    Write-Output "*** Installed ODBC drivers:"
    Get-OdbcDriver
}


# Microsoft SQL Server
# AppVeyor build servers are always 64-bit and only the 64-bit SQL Server ODBC
# driver msi files can be installed on them.  However, the 64-bit msi files include
# both 32-bit and 64-bit drivers anyway.

# The "SQL Server Native Client 10.0" and "SQL Server Native Client 11.0" driver
# downloads do not appear to be available.

CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 11 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/5/7/2/57249A3A-19D6-4901-ACCE-80924ABEB267/ENU/x64/msodbcsql.msi" `
    -msifile_path "$cache_dir\msodbcsql_11.0.0.0_x64.msi" `
    -msiexec_paras @("IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL");

# With the 13.0 driver, some tests fail for Python 2.7 so using version 13.1.
# 13.0: https://download.microsoft.com/download/1/E/7/1E7B1181-3974-4B29-9A47-CC857B271AA2/English/X64/msodbcsql.msi
# 13.1: https://download.microsoft.com/download/D/5/E/D5EEF288-A277-45C8-855B-8E2CB7E25B96/x64/msodbcsql.msi
CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 13 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/D/5/E/D5EEF288-A277-45C8-855B-8E2CB7E25B96/x64/msodbcsql.msi" `
    -msifile_path "$cache_dir\msodbcsql_13.1.0.0_x64.msi" `
    -msiexec_paras @("IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL");

CheckAndInstallMsiFromUrl `
    -driver_name "ODBC Driver 17 for SQL Server" `
    -driver_bitness "64-bit" `
    -driver_url "https://download.microsoft.com/download/E/6/B/E6BFDC7A-5BCD-4C51-9912-635646DA801E/en-US/msodbcsql_17.5.1.1_x64.msi" `
    -msifile_path "$cache_dir\msodbcsql_17.5.1.1_x64.msi" `
    -msiexec_paras @("IACCEPTMSODBCSQLLICENSETERMS=YES", "ADDLOCAL=ALL");
    
if ($python_arch -eq "64") {

    CheckAndInstallZippedMsiFromUrl `
        -driver_name "PostgreSQL Unicode(x64)" `
        -driver_bitness "64-bit" `
        -driver_url "https://ftp.postgresql.org/pub/odbc/versions/msi/psqlodbc_09_06_0500-x64.zip" `
        -zipfile_path "$temp_dir\psqlodbc_09_06_0500-x64.zip" `
        -zip_internal_msi_file "psqlodbc_x64.msi" `
        -msifile_path "$cache_dir\psqlodbc_09_06_0500-x64.msi";

    # MySQL 8.0 drivers apparently don't work on Python 2.7 ("system error 126").
    # Note, installing MySQL 8.0 ODBC drivers causes the 5.3 drivers to be uninstalled.
    if ($python_major_version -eq "2") {
        CheckAndInstallMsiFromUrl `
            -driver_name "MySQL ODBC 5.3 ANSI Driver" `
            -driver_bitness "64-bit" `
            -driver_url "https://dev.mysql.com/get/Downloads/Connector-ODBC/5.3/mysql-connector-odbc-5.3.14-winx64.msi" `
            -msifile_path "$cache_dir\mysql-connector-odbc-5.3.14-winx64.msi";
    } else {
        CheckAndInstallMsiFromUrl `
            -driver_name "MySQL ODBC 8.0 ANSI Driver" `
            -driver_bitness "64-bit" `
            -driver_url "https://dev.mysql.com/get/Downloads/Connector-ODBC/8.0/mysql-connector-odbc-8.0.19-winx64.msi" `
            -msifile_path "$cache_dir\mysql-connector-odbc-8.0.19-winx64.msi";
    }

} elseif ($python_arch -eq "32") {

    CheckAndInstallZippedMsiFromUrl `
        -driver_name "PostgreSQL Unicode" `
        -driver_bitness "32-bit" `
        -driver_url "https://ftp.postgresql.org/pub/odbc/versions/msi/psqlodbc_09_06_0500-x86.zip" `
        -zipfile_path "$temp_dir\psqlodbc_09_06_0500-x86.zip" `
        -zip_internal_msi_file "psqlodbc_x86.msi" `
        -msifile_path "$cache_dir\psqlodbc_09_06_0500-x86.msi";

    # MySQL 8.0 drivers apparently don't work on Python 2.7 ("system error 126").
    # Note, installing MySQL 8.0 ODBC drivers causes the 5.3 drivers to be uninstalled.
    if ($python_major_version -eq 2) {
        CheckAndInstallMsiFromUrl `
            -driver_name "MySQL ODBC 5.3 ANSI Driver" `
            -driver_bitness "32-bit" `
            -driver_url "https://dev.mysql.com/get/Downloads/Connector-ODBC/5.3/mysql-connector-odbc-5.3.14-win32.msi" `
            -msifile_path "$cache_dir\mysql-connector-odbc-5.3.14-win32.msi";
    } else {
            CheckAndInstallMsiFromUrl `
            -driver_name "MySQL ODBC 8.0 ANSI Driver" `
            -driver_bitness "32-bit" `
            -driver_url "https://dev.mysql.com/get/Downloads/Connector-ODBC/8.0/mysql-connector-odbc-8.0.19-win32.msi" `
            -msifile_path "$cache_dir\mysql-connector-odbc-8.0.19-win32.msi";
    }
} else {
    Write-Output "ERROR: Unexpected Python architecture:"
    Write-Output $python_arch
}


If (${env:APVYR_VERBOSE} -eq "true") {
    Write-Output ""
    Write-Output "*** Contents of the cache directory: $cache_dir"
    Get-ChildItem $cache_dir
    Write-Output ""
    Write-Output "*** Contents of the temporary directory: $temp_dir"
    Get-ChildItem $temp_dir
    Write-Output ""
    Write-Output "*** Installed ODBC drivers:"
    Get-OdbcDriver
}


# To compile Python 3.5 on VS 2019, we have to copy some files into Visual Studio 14.0
# otherwise we get an error on the build as follows:
#   LINK : fatal error LNK1158: cannot run 'rc.exe'
# See: https://stackoverflow.com/a/52580041
if ($python_major_version -eq "3" -And $python_minor_version -eq "5") {
    if ("$env:APPVEYOR_BUILD_WORKER_IMAGE" -eq "Visual Studio 2019") {
        Write-Output ""
        Write-Output "*** Copy rc files from Windows Kits into Visual Studio 14.0"
        Copy-Item "C:\Program Files (x86)\Windows Kits\10\bin\10.0.17134.0\x64\rc.exe"    "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\bin\"
        Copy-Item "C:\Program Files (x86)\Windows Kits\10\bin\10.0.17134.0\x64\rcdll.dll" "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\bin\"
    }
}
