# Paddle2ONNX
ONNX Model Exporter for PaddlePaddle

## 获取代码
```
mkdir /root/build_paddle2onnx
cd /root/build_paddle2onnx
git clone https://github.com/PaddlePaddle/Paddle2ONNX.git
cd Paddle2ONNX
git checkout cpp
git submodule init
git submodule update
```

## 编译可执行二进制

### Linux
如无安装protobuf，需先行安装
```
cd /root/build_paddle2onnx
git clone https://github.com/protocolbuffers/protobuf.git
cd protobuf
git checkout v3.16.0
./autogen.sh
./configure CFLAGS="-FPIC" CXXFLAGS="-fPIC" --prefix=/root/build_paddle2onnx/installed_protobuf
make -j8
make install

# 编译完成后，可直接替换系统中原有protobuf
rm -rf /usr/local/lib/libproto*
cp /root/build_paddle2onnx/installed_protobuf/lib/* /usr/local/lib/
cp /root/build_paddle2onnx/installed_protobuf/bin/protoc /usr/local/bin/protoc
cp /root/build_paddle2onnx/installed_protobuf/include/* /usr/local/include
```

编译paddle2onnx二进制
```
cd /root/build_paddle2onnx/Paddle2ONNX
mkdir build
cd build
cmake ..
make -j8
```

### MacOS
首先安装编译工具
```
brew update
brew install autoconf && brew install automake && brew install cmake
```

编译安装protobuf
```
cd /root/build_paddle2onnx/
wget https://github.com/protocolbuffers/protobuf/releases/download/v3.16.0/protobuf-cpp-3.16.0.tar.gz
tar -xvf protobuf-cpp-3.16.0.tar.gz
cd protobuf-3.16.0
mkdir build_source && cd build_source
cmake ../cmake -Dprotobuf_BUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -Dprotobuf_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
make -j8
make install
```

编译二进制
```
cd /root/build_paddle2onnx/Paddle2ONNX
mkdir build
cd build
cmake ..
make -j8
```

### Windows
注意Windows编译先验条件是已经安装Visual Studio 2019，编译时请使用系统提供的命令行工具`x64 Native Tools Command Prompt for VS 2019`

首先编译安装protobuf
```
cd /root/build_paddle2onnx/
git clone https://github.com/protocolbuffers/protobuf.git
cd protobuf
git checkout v3.16.0
cd cmake
cmake -G "Visual Studio 16 2019" -A x64 -DCMAKE_INSTALL_PREFIX=/root/build_paddle2onnx/installed_protobuf -Dprotobuf_MSVC_STATIC_RUNTIME=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_EXAMPLES=OFF .
msbuild protobuf.sln /m /p:Configuration=Release /p:Platform=x64
msbuild INSTALL.vcxproj /p:Configuration=Release /p:Platform=x64
```

编译完成后，将protobuf路径添加到环境变量中
```
set PATH=/root/build_paddle2onnx/installed_protobuf/bin;%PATH%
```

接下来即可开始编译二进制
```
cd /root/build_paddle2onnx/Paddle2ONNX
mkdir build
cd build
cmake -G "Visual Studio 16 2019" -A x64 ..
msbuild paddle2onnx.sln /m /p:Configuration=Release /p:Platform=x64
```
编译完成的二进制在`build/Release`目录下

## 测试二进制
由于目前编译时使用了`v1.10.2`的onnx，因此我们也必须使用`onnxruntime`最新版本来验证转换后onnx模型的正确性
```
pip install paddlepaddle --upgrade
pip install onnxruntime --upgrade
```

使用如下代码生成一个简单的卷积网络模型，保存在`conv`目录下
```
import paddle
model = paddle.nn.Conv2D(3, 5, 3, bias_attr=False)
model.eval()
input = paddle.static.InputSpec(shape=[2, 3, 224, 224], dtype='float32', name='x')
paddle.jit.save(model, "conv/model", [input])
```

使用二进制将Paddle模型转换ONNX模型，转换后的模型在本地目录下的`model.onnx`文件
```
./p2o_exec conv/model.pdmodel conv/model.pdiparams
```

使用如下脚本验证两个模型的预测结果是否正确
```
import paddle
import onnxruntime as ort
import numpy as np

model0 = paddle.jit.load("conv/model")
model1 = ort.InferenceSession("model.onnx")

data = np.random.rand(2, 3, 224, 224).astype("float32")

result0 = model0(paddle.to_tensor(data)).numpy()
result1, = model1.run(None, {"x": data})
diff = result1 - result0
print(diff.max(), diff.min())
```

## Python安装
注意也同可执行二进制程序编译一样，需要提前安装好protobuf

当前仅支持Linux/Mac（如果你是Mac M1 Chip，请参考[M1 Chip下的安装说明](mac_m1_chip.md)）
```
pip install onnx --upgrade
cd /root/build_paddle2onnx/Paddle2ONNX
python setup.py install
```
使用方式如下
```
import paddle2onnx
model_filename = "model/model.pdmodel"
params_filename = 'model/model.pdiparams"
save_path = "model/model.onnx"

paddle2onnx.export(model_filename, params_filename, save_path, opset_version=11)
```

## OP开发
> OP开发相关指导详见：[OP开发指南](docs/zh/Paddle2ONNX_Development_Guide.md)
