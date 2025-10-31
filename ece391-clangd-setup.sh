#!/bin/bash
# Define local binary directory
LOCAL_BIN_DIR="$HOME/.local/bin"
CONFIG_FILE=".clangd"

# Install Clangd release binary 20.1.0
echo "Start downloading Clangd binary from GitHub releases..."
wget -nv "https://github.com/clangd/clangd/releases/download/21.1.0/clangd-linux-21.1.0.zip"
echo "Unzipping release zip file"
unzip -q clangd-linux-21.1.0.zip

# Create local bin directory if it does not exist
if [ ! -d "$LOCAL_BIN_DIR" ]; then
  echo "Directory '$LOCAL_BIN_DIR' not found. Creating it now..."
  mkdir -p "$LOCAL_BIN_DIR"
  echo "Directory created successfully."
else
  echo "Directory '$LOCAL_BIN_DIR' already exists. Skipping this step."
fi

# Move the Clangd binary to local binaries
echo "Moving the binary to local bin..."
mv ./clangd_21.1.0/bin/clangd $LOCAL_BIN_DIR

# Set up compiledb
echo "Installing compiledb..."
pip install -U -q compiledb
echo "Successfully installed compiledb."

# Remove temporary install files
echo "Deleting temporary install files..."
rm -rf clangd_21.1.0
rm clangd-linux-21.1.0.zip

# Generate .clangd config file
# We need to tell clangd what libaries we are using and what target we are compiling to
# We are also removing the mno-riscv-attribute flag 
echo "Generating clangd config file"
cat > "$CONFIG_FILE" << EOF
CompileFlags:
  Remove: [-mno-riscv-attribute]
  Add:
    - --target=riscv64-unknown-elf
    - -I/class/ece391/rhel9/lib/gcc/riscv64-unknown-elf/13.2.0/include
    - -I/class/ece391/rhel9/lib/gcc/riscv64-unknown-elf/13.2.0/include-fixed
    - -I/class/ece391/rhel9/lib/gcc/riscv64-unknown-elf/13.2.0/../../../../riscv64-unknown-elf/include
EOF

# Manual installation steps
echo "Please copy the following path and follow the rest of the steps of the instructions!"
echo "$LOCAL_BIN_DIR/clangd"