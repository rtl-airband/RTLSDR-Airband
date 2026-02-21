# System Tests

This directory contains signal replay tests for the RTLSDR-Airband project. Tests are configured via JSON files that reference:

- The RTLSDR-Airband configuration files (`.conf`)
- Input files (`.dat`)
- Output expectations

NOTE: When a test runs, the `rtl_airband` binary executes with its working directory set to the test output directory:

```
system_tests/outputs/{BuildName}_{TestName}/
```

The test runner automatically creates **symlinks** for all data files listed in the JSON file's `data_files` array into this output directory. Therefore, **all paths in the `.conf` file should use `./` (current working directory) for both input and output files**.

## Directory Structure

```
system_tests/
├── run                 # Python test runner script
├── builds/             # Build output directories (created at runtime)
│   ├── {BuildName}_{TestName}/
└── outputs/            # Test output directories (created at runtime)
    └── {BuildName}_{TestName}/
        ├── *.dat       # Symlinks to data files (created by test runner)
        ├── *.mp3       # Generated MP3 output files
        ├── *.prom      # Prometheus metrics files
        └── *.log       # Debug log files
```


## Basic Usage

```bash
./system_tests/run <path-to-json-config-file>
```


## Test Cases

### JSON Test Configuration File

The JSON configuration file defines which builds to compile and which test cases to run. It has two main sections: `builds` and `test_cases`.


#### JSON Field Descriptions

**builds:**
- `BuildName`: A unique name for the build configuration
  - Array of CMake arguments to pass to cmake configuration

**test_cases:**
- `mode_name`: The modulation mode ("am" or "nfm")
  - "nfm" tests only run with NFM-enabled builds (builds with `-DNFM=TRUE` in the CMake argument list)
  - "am" tests run with all builds
- `config`: Filename of the `.conf` file (must be in the same directory as the JSON file)
- `data_files`: Array of input data filenames (must be in the same directory as the JSON file)
  - These files will be symlinked into the output directory before the test runs
  - The `.conf` file should reference these files in the current directory (e.g., `"./sample_data.dat"`)
- `mp3_files`: Expected MP3 output files and their properties
  - Key: glob pattern to match output files (e.g., `"output_*.mp3"`)
  - `duration`: expected duration in seconds (tolerance: ±1% or ±0.75s, whichever is greater)
  - `mode`: "mono" or "stereo"
- `other_files`: List of expected output files (not MP3s)

#### Structure

```json
{
    "builds": {
        "BuildName": [
            "cmake_arg1",
            "cmake_arg2"
        ]
    },
    "test_cases": {
        "mode_name": [
            {
                "config": "config_filename.conf",
                "data_files": [
                    "input_file1.dat",
                    "input_file2.dat"
                ],
                "mp3_files": {
                    "output_pattern_*.mp3": {
                        "duration": 10.5,
                        "mode": "mono"
                    }
                },
                "other_files": [
                    "filename.ext"
                ]
            }
        ]
    }
}
```


### Example Directory Structure

```
├── test_cases/
│   ├── *.json          # Test configuration files (define builds and test cases)
│   ├── *.conf          # rtl_airband configuration files for specific test scenarios
│   └── *.dat           # Sample data files for file-based input sources
```


### Example JSON File

Here's a minimal example configuration (`simple_test.json`):

```json
{
    "builds": {
        "Debug": [
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DBUILD_UNITTESTS=TRUE"
        ],
        "Release": [
            "-DCMAKE_BUILD_TYPE=Release",
            "-DBUILD_UNITTESTS=TRUE"
        ]
    },
    "test_cases": {
        "am": [
            {
                "config": "simple_receiver.conf",
                "data_files": [
                    "sample_data.dat"
                ],
                "mp3_files": {
                    "output_*.mp3": {
                        "duration": 5.0,
                        "mode": "mono"
                    }
                },
                "other_files": [
                    "radio.prom"
                ]
            }
        ]
    }
}
```

### Example Configuration File

Here's a basic example (`simple_receiver.conf`):

```
fft_size = 1024;
localtime = true;
stats_filepath = "./radio.prom";

mixers: {
    main_mixer: {
        outputs: (
            {
                type = "file";
                directory = "./";
                continuous = false;
                filename_template = "mixer_output";
            }
        );
    }
};

devices:
(
    {
        type = "file";
        centerfreq = 154.95;
        sample_rate = 2.40;
        filepath = "./sample_data.dat";
        speedup_factor = 8;
        channels:
        (
            {
                freq = 154.25000;
                label = "Test Channel";
                modulation = "nfm";
                lowpass = -1;
                highpass = -1;
                outputs:
                (
                    {
                        type = "file";
                        directory = "./";
                        continuous = false;
                        filename_template = "output";
                    },
                    {
                        type = "mixer";
                        name = "main_mixer";
                    }
                );
            }
        );
    }
);
```
