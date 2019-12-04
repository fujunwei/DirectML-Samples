// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "pch.h"
#include <windows.h> 

using winrt::com_ptr;
using winrt::check_hresult;
using winrt::check_bool;
using winrt::handle;

#define USE_VPU 1
bool g_support_f16 = 1;

const uint16_t f16_val = 11879; // 0.10003662109375
const FLOAT f32_val = 0.10003662109375;

std::string DriverDescription(com_ptr<IDXCoreAdapter>& adapter, bool selected = false) {
	// If the adapter is a software adapter then don't consider it for index selection
	size_t driverDescriptionSize;
	check_hresult(adapter->GetPropertySize(DXCoreAdapterProperty::DriverDescription,
		&driverDescriptionSize));
	CHAR* driverDescription = new CHAR[driverDescriptionSize];
	check_hresult(adapter->GetProperty(DXCoreAdapterProperty::DriverDescription,
		driverDescriptionSize, driverDescription));
	if (selected) {
		printf("Use adapter : %s\n", driverDescription);
	}

	std::string driverDescriptionStr = std::string(driverDescription);
	free(driverDescription);

	return driverDescriptionStr;
}

void InitWithDXCore(com_ptr<ID3D12Device>& d3D12Device,
	com_ptr<ID3D12CommandQueue>& commandQueue,
	com_ptr<ID3D12CommandAllocator>& commandAllocator,
	com_ptr<ID3D12GraphicsCommandList>& commandList) {
	HMODULE library = nullptr;
	library = LoadLibrary(L"dxcore.dll");
	if (!library) {
		//throw hresult_invalid_argument(L"DXCore isn't support on this manchine.");
		std::wcout << L"DXCore isn't support on this manchine. ";
		return;
	}

	com_ptr<IDXCoreAdapterFactory> adapterFactory;
	check_hresult(DXCoreCreateAdapterFactory(IID_PPV_ARGS(adapterFactory.put())));

	com_ptr<IDXCoreAdapterList> adapterList;
	const GUID dxGUIDs[] = { DXCORE_ADAPTER_ATTRIBUTE_D3D12_CORE_COMPUTE };

	check_hresult(
		adapterFactory->CreateAdapterList(ARRAYSIZE(dxGUIDs), dxGUIDs, IID_PPV_ARGS(adapterList.put())));

	com_ptr<IDXCoreAdapter> currAdapter = nullptr;
	IUnknown* pAdapter = nullptr;
	com_ptr<IDXGIAdapter> dxgiAdapter;
	D3D_FEATURE_LEVEL d3dFeatureLevel = D3D_FEATURE_LEVEL_1_0_CORE;
	D3D12_COMMAND_LIST_TYPE commandQueueType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
	for (UINT i = 0; i < adapterList->GetAdapterCount(); i++) {
		currAdapter = nullptr;
		check_hresult(adapterList->GetAdapter(i, currAdapter.put()));

		bool isHardware;
		check_hresult(currAdapter->GetProperty(DXCoreAdapterProperty::IsHardware, &isHardware));
#if USE_VPU == 1
		std::string adapterNameStr = "VPU";
		std::string driverDescriptionStr = DriverDescription(currAdapter);
		std::transform(driverDescriptionStr.begin(), driverDescriptionStr.end(),
			driverDescriptionStr.begin(), ::tolower);
		std::transform(adapterNameStr.begin(), adapterNameStr.end(), adapterNameStr.begin(),
			::tolower);
		if (isHardware && strstr(driverDescriptionStr.c_str(), adapterNameStr.c_str())) {
			pAdapter = currAdapter.get();
			break;
		}
#else
	// Check if adapter selected has DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS attribute selected. If
	// so, then GPU was selected that has D3D12 and D3D11 capabilities. It would be the most stable
	// to use DXGI to enumerate GPU and use D3D_FEATURE_LEVEL_11_0 so that image tensorization for
	// video frames would be able to happen on the GPU.
	if (isHardware && currAdapter->IsAttributeSupported(DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS)) {
		d3dFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0;
		com_ptr<IDXGIFactory4> dxgiFactory4;
		HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory4.put()));
		if (hr == S_OK) {
			// If DXGI factory creation was successful then get the IDXGIAdapter from the LUID
			// acquired from the selectedAdapter
			LUID adapterLuid;
			check_hresult(currAdapter->GetProperty(DXCoreAdapterProperty::InstanceLuid, &adapterLuid));
			check_hresult(dxgiFactory4->EnumAdapterByLuid(adapterLuid, __uuidof(IDXGIAdapter),
				dxgiAdapter.put_void()));
			pAdapter = dxgiAdapter.get();
			break;
		}
	}
#endif
	}

	if (currAdapter == nullptr) {
		std::wcout << L"ERROR: No matching adapter with given adapter name: ";
		return;
	}
	DriverDescription(currAdapter, true);

	// create D3D12Device
	check_hresult(
		D3D12CreateDevice(pAdapter, d3dFeatureLevel, __uuidof(ID3D12Device), d3D12Device.put_void()));

	// create D3D12 command queue from device
	//com_ptr<ID3D12CommandQueue> d3d12CommandQueue;
	D3D12_COMMAND_QUEUE_DESC commandQueueDesc = {};
	commandQueueDesc.Type = commandQueueType;
	check_hresult(d3D12Device->CreateCommandQueue(&commandQueueDesc, __uuidof(ID3D12CommandQueue),
		commandQueue.put_void()));

	check_hresult(d3D12Device->CreateCommandAllocator(
		commandQueueType,
		__uuidof(commandAllocator),
		commandAllocator.put_void()));

	check_hresult(d3D12Device->CreateCommandList(
		0,
		commandQueueType,
		commandAllocator.get(),
		nullptr,
		__uuidof(commandList),
		commandList.put_void()));
}

void CloseExecuteResetWait(
	com_ptr<ID3D12Device> d3D12Device,
	com_ptr<ID3D12CommandQueue> commandQueue,
	com_ptr<ID3D12CommandAllocator> commandAllocator,
	com_ptr<ID3D12GraphicsCommandList> commandList)
{
	check_hresult(commandList->Close());

	ID3D12CommandList* commandLists[] = { commandList.get() };
	commandQueue->ExecuteCommandLists(ARRAYSIZE(commandLists), commandLists);

	check_hresult(commandList->Reset(commandAllocator.get(), nullptr));

	com_ptr<ID3D12Fence> d3D12Fence;
	check_hresult(d3D12Device->CreateFence(
		0,
		D3D12_FENCE_FLAG_NONE,
		_uuidof(d3D12Fence),
		d3D12Fence.put_void()));

	handle fenceEventHandle{ 0 };
	fenceEventHandle.attach(::CreateEvent(nullptr, true, false, nullptr));
	check_bool(bool{ fenceEventHandle });

	check_hresult(d3D12Fence->SetEventOnCompletion(1, fenceEventHandle.get()));

	check_hresult(commandQueue->Signal(d3D12Fence.get(), 1));
	::WaitForSingleObjectEx(fenceEventHandle.get(), INFINITE, FALSE);
}

// ===================================================================================================================
//   DML utilities
// ===================================================================================================================

inline UINT64 DMLCalcBufferTensorSize(
	DML_TENSOR_DATA_TYPE dataType,
	UINT dimensionCount,
	_In_reads_(dimensionCount) const UINT* sizes,
	_In_reads_opt_(dimensionCount) const UINT* strides
)
{
	UINT elementSizeInBytes = 0;
	switch (dataType)
	{
	case DML_TENSOR_DATA_TYPE_FLOAT32:
	case DML_TENSOR_DATA_TYPE_UINT32:
	case DML_TENSOR_DATA_TYPE_INT32:
		elementSizeInBytes = 4;
		break;

	case DML_TENSOR_DATA_TYPE_FLOAT16:
	case DML_TENSOR_DATA_TYPE_UINT16:
	case DML_TENSOR_DATA_TYPE_INT16:
		elementSizeInBytes = 2;
		break;

	case DML_TENSOR_DATA_TYPE_UINT8:
	case DML_TENSOR_DATA_TYPE_INT8:
		elementSizeInBytes = 1;
		break;

	default:
		return 0; // Invalid data type
	}

	UINT64 minimumImpliedSizeInBytes = 0;
	if (!strides)
	{
		minimumImpliedSizeInBytes = sizes[0];
		for (UINT i = 1; i < dimensionCount; ++i)
		{
			minimumImpliedSizeInBytes *= sizes[i];
		}
		minimumImpliedSizeInBytes *= elementSizeInBytes;
	}
	else
	{
		UINT indexOfLastElement = 0;
		for (UINT i = 0; i < dimensionCount; ++i)
		{
			indexOfLastElement += (sizes[i] - 1) * strides[i];
		}

		minimumImpliedSizeInBytes = (indexOfLastElement + 1) * elementSizeInBytes;
	}

	// Round up to the nearest 4 bytes.
	minimumImpliedSizeInBytes = (minimumImpliedSizeInBytes + 3) & ~3ui64;

	return minimumImpliedSizeInBytes;
}

int __cdecl wmain(int /*argc*/, char** /*argv*/)
{
	com_ptr<ID3D12Device> d3D12Device;
	com_ptr<ID3D12CommandQueue> commandQueue;
	com_ptr<ID3D12CommandAllocator> commandAllocator;
	com_ptr<ID3D12GraphicsCommandList> commandList;

	std::wcout << "DML Conv2D Test" << std::endl;

	// Set up Direct3D 12.
	InitWithDXCore(d3D12Device, commandQueue, commandAllocator, commandList);

	// Create the DirectML device.

	DML_CREATE_DEVICE_FLAGS dmlCreateDeviceFlags = DML_CREATE_DEVICE_FLAG_NONE;

#if defined (_DEBUG)
	// If the project is in a debug build, then enable debugging via DirectML debug layers with this flag.
	//dmlCreateDeviceFlags |= DML_CREATE_DEVICE_FLAG_DEBUG;
#endif

	com_ptr<IDMLDevice> dmlDevice;
	check_hresult(DMLCreateDevice(
		d3D12Device.get(),
		dmlCreateDeviceFlags,
		__uuidof(dmlDevice),
		dmlDevice.put_void()));

	DML_FEATURE_DATA_TENSOR_DATA_TYPE_SUPPORT support_f16 = { false };
	DML_FEATURE_QUERY_TENSOR_DATA_TYPE_SUPPORT fp16_query = {
		DML_TENSOR_DATA_TYPE_FLOAT16 };
	check_hresult(dmlDevice->CheckFeatureSupport(
		DML_FEATURE_TENSOR_DATA_TYPE_SUPPORT, sizeof(fp16_query), &fp16_query,
		sizeof(support_f16), &support_f16));

	std::wcout << "Request float16: " << g_support_f16 << std::endl;
	std::wcout << "Support float16: " << support_f16.IsSupported << std::endl;

	if (g_support_f16 && !support_f16.IsSupported) {
		std::wcout << "Request float16 failed" << std::endl;
		return -1;
	}

	// The command recorder is a stateless object that records Dispatches into an existing Direct3D 12 command list.
	com_ptr<IDMLCommandRecorder> dmlCommandRecorder;
	check_hresult(dmlDevice->CreateCommandRecorder(
		__uuidof(dmlCommandRecorder),
		dmlCommandRecorder.put_void()));

	constexpr UINT inputSizes[4] = { 1, 512, 13, 13 };
	std::wcout << "Input tensor shape: [" << inputSizes[0] << ", " << inputSizes[1] << ", " << inputSizes[2] << ", " << inputSizes[3] << "]" << std::endl;
	constexpr UINT inputElementCount = inputSizes[0] * inputSizes[1] * inputSizes[2] * inputSizes[3];
	DML_BUFFER_TENSOR_DESC inputTensorDesc = {};
	inputTensorDesc.DataType = g_support_f16 ? DML_TENSOR_DATA_TYPE_FLOAT16
		: DML_TENSOR_DATA_TYPE_FLOAT32;
	inputTensorDesc.Flags = DML_TENSOR_FLAG_NONE;
	inputTensorDesc.DimensionCount = ARRAYSIZE(inputSizes);
	inputTensorDesc.Sizes = inputSizes;
	inputTensorDesc.Strides = nullptr;
	inputTensorDesc.TotalTensorSizeInBytes = DMLCalcBufferTensorSize(
		inputTensorDesc.DataType,
		inputTensorDesc.DimensionCount,
		inputTensorDesc.Sizes,
		inputTensorDesc.Strides);

	constexpr UINT weightSizes[4] = { 1000, 512, 1, 1 };
	std::wcout << "Weight tensor shape: [" << weightSizes[0] << ", " << weightSizes[1] << ", " << weightSizes[2] << ", " << weightSizes[3] << "]" << std::endl;
	constexpr UINT weightElementCount = weightSizes[0] * weightSizes[1] * weightSizes[2] * weightSizes[3];
	DML_BUFFER_TENSOR_DESC weightTensorDesc = {};
	weightTensorDesc.DataType = g_support_f16 ? DML_TENSOR_DATA_TYPE_FLOAT16
		: DML_TENSOR_DATA_TYPE_FLOAT32;
	weightTensorDesc.Flags = DML_TENSOR_FLAG_OWNED_BY_DML;
	weightTensorDesc.DimensionCount = ARRAYSIZE(weightSizes);
	weightTensorDesc.Sizes = weightSizes;
	weightTensorDesc.Strides = nullptr;
	weightTensorDesc.TotalTensorSizeInBytes = DMLCalcBufferTensorSize(
		weightTensorDesc.DataType,
		weightTensorDesc.DimensionCount,
		weightTensorDesc.Sizes,
		weightTensorDesc.Strides);

	constexpr UINT biasSizes[4] = { 1, 1000, 1, 1 };
	std::wcout << "Bias tensor shape: [" << biasSizes[0] << ", " << biasSizes[1] << ", " << biasSizes[2] << ", " << biasSizes[3] << "]" << std::endl;
	constexpr UINT biasElementCount = biasSizes[0] * biasSizes[1] * biasSizes[2] * biasSizes[3];
	DML_BUFFER_TENSOR_DESC biasTensorDesc = {};
	biasTensorDesc.DataType = g_support_f16 ? DML_TENSOR_DATA_TYPE_FLOAT16
		: DML_TENSOR_DATA_TYPE_FLOAT32;
	biasTensorDesc.Flags = DML_TENSOR_FLAG_OWNED_BY_DML;
	biasTensorDesc.DimensionCount = ARRAYSIZE(biasSizes);
	biasTensorDesc.Sizes = biasSizes;
	biasTensorDesc.Strides = nullptr;
	biasTensorDesc.TotalTensorSizeInBytes = DMLCalcBufferTensorSize(
		biasTensorDesc.DataType,
		biasTensorDesc.DimensionCount,
		biasTensorDesc.Sizes,
		biasTensorDesc.Strides);

	constexpr UINT outputSizes[4] = { 1, 1000, 13, 13 };
	std::wcout << "Output tensor shape: [" << outputSizes[0] << ", " << outputSizes[1] << ", " << outputSizes[2] << ", " << outputSizes[3] << "]" << std::endl;
	constexpr UINT outputElementCount = outputSizes[0] * outputSizes[1] * outputSizes[2] * outputSizes[3];
	DML_BUFFER_TENSOR_DESC outputTensorDesc = {};
	outputTensorDesc.DataType = g_support_f16 ? DML_TENSOR_DATA_TYPE_FLOAT16
		: DML_TENSOR_DATA_TYPE_FLOAT32;
	outputTensorDesc.Flags = DML_TENSOR_FLAG_NONE;
	outputTensorDesc.DimensionCount = ARRAYSIZE(outputSizes);
	outputTensorDesc.Sizes = outputSizes;
	outputTensorDesc.Strides = nullptr;
	outputTensorDesc.TotalTensorSizeInBytes = DMLCalcBufferTensorSize(
		outputTensorDesc.DataType,
		outputTensorDesc.DimensionCount,
		outputTensorDesc.Sizes,
		outputTensorDesc.Strides);
	com_ptr<IDMLOperator> dmlOperator;
	{
		// Create DirectML operator(s). Operators represent abstract functions such as "multiply", "reduce", "convolution", or even
		// compound operations such as recurrent neural nets. This example creates an instance of the Identity operator,
		// which applies the function f(x) = x for all elements in a tensor.
		DML_TENSOR_DESC input_tensor_desc = { DML_TENSOR_TYPE_BUFFER,
											 &inputTensorDesc };

		DML_TENSOR_DESC weights_tensor_desc = { DML_TENSOR_TYPE_BUFFER,
											   &weightTensorDesc };

		DML_TENSOR_DESC bias_tensor_desc = { DML_TENSOR_TYPE_BUFFER,
											   & biasTensorDesc };

		DML_TENSOR_DESC output_tensor_desc = { DML_TENSOR_TYPE_BUFFER,
											  &outputTensorDesc };

		const uint32_t strides[2] = { 1, 1 };
		const uint32_t dilations[2] = { 1, 1 };
		const uint32_t start_padding[2] = { 0, 0 };
		const uint32_t end_padding[2] = { 0, 0 };
		const uint32_t output_padding[2] = { 0, 0 };

		DML_CONVOLUTION_OPERATOR_DESC conv_operator_desc = {
			&input_tensor_desc,
			&weights_tensor_desc,
			&bias_tensor_desc,
			&output_tensor_desc,
			DML_CONVOLUTION_MODE_CROSS_CORRELATION,
			DML_CONVOLUTION_DIRECTION_FORWARD,
			2,
			strides,
			dilations,
			start_padding,
			end_padding,
			output_padding,
			1,
			nullptr };
		DML_OPERATOR_DESC operator_desc = { DML_OPERATOR_CONVOLUTION,
										   &conv_operator_desc };

		check_hresult(dmlDevice->CreateOperator(
			&operator_desc,
			__uuidof(dmlOperator),
			dmlOperator.put_void()));
	}

	// Compile the operator into an object that can be dispatched to the GPU. In this step, DirectML performs operator
	// fusion and just-in-time (JIT) compilation of shader bytecode, then compiles it into a Direct3D 12 pipeline state object (PSO).
	// The resulting compiled operator is a baked, optimized form of an operator suitable for execution on the GPU.

	com_ptr<IDMLCompiledOperator> dmlCompiledOperator;
	check_hresult(dmlDevice->CompileOperator(
		dmlOperator.get(),
		g_support_f16 ? DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION
			: DML_EXECUTION_FLAG_NONE,
		__uuidof(dmlCompiledOperator),
		dmlCompiledOperator.put_void()));

	com_ptr<IDMLOperatorInitializer> dmlOperatorInitializer;
	IDMLCompiledOperator* dmlCompiledOperators[] = { dmlCompiledOperator.get() };
	check_hresult(dmlDevice->CreateOperatorInitializer(
		ARRAYSIZE(dmlCompiledOperators),
		dmlCompiledOperators,
		__uuidof(dmlOperatorInitializer),
		dmlOperatorInitializer.put_void()));

	// Query the operator for the required size (in descriptors) of its binding table.
	// You need to initialize an operator exactly once before it can be executed, and
	// the two stages require different numbers of descriptors for binding. For simplicity,
	// we create a single descriptor heap that's large enough to satisfy them both.
	DML_BINDING_PROPERTIES initializeBindingProperties = dmlOperatorInitializer->GetBindingProperties();
	DML_BINDING_PROPERTIES executeBindingProperties = dmlCompiledOperator->GetBindingProperties();
	UINT descriptorCount = std::max(
		initializeBindingProperties.RequiredDescriptorCount,
		executeBindingProperties.RequiredDescriptorCount);

	// Create descriptor heaps.
	com_ptr<ID3D12DescriptorHeap> descriptorHeap;

	D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
	descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	descriptorHeapDesc.NumDescriptors = descriptorCount;
	descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	check_hresult(d3D12Device->CreateDescriptorHeap(
		&descriptorHeapDesc,
		_uuidof(descriptorHeap),
		descriptorHeap.put_void()));

	// Set the descriptor heap(s).
	ID3D12DescriptorHeap* d3D12DescriptorHeaps[] = { descriptorHeap.get() };
	commandList->SetDescriptorHeaps(ARRAYSIZE(d3D12DescriptorHeaps), d3D12DescriptorHeaps);

	// Create a binding table over the descriptor heap we just created.
	DML_BINDING_TABLE_DESC dmlBindingTableDesc{};
	dmlBindingTableDesc.Dispatchable = dmlOperatorInitializer.get();
	dmlBindingTableDesc.CPUDescriptorHandle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
	dmlBindingTableDesc.GPUDescriptorHandle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
	dmlBindingTableDesc.SizeInDescriptors = descriptorCount;

	com_ptr<IDMLBindingTable> dmlBindingTable;
	check_hresult(dmlDevice->CreateBindingTable(
		&dmlBindingTableDesc,
		__uuidof(dmlBindingTable),
		dmlBindingTable.put_void()));

	// Create the temporary and persistent resources that are necessary for executing an operator.

	// The temporary resource is scratch memory (used internally by DirectML), whose contents you don't need to define.
	// The persistent resource is long-lived, and you need to initialize it using the IDMLOperatorInitializer.

	UINT64 temporaryResourceSize = std::max(
		initializeBindingProperties.TemporaryResourceSize,
		executeBindingProperties.TemporaryResourceSize);
	UINT64 persistentResourceSize = executeBindingProperties.PersistentResourceSize;

	// Bind and initialize the operator on the GPU.


	// Binding Inputs
	// weights.
	UINT64 weightBufferSize{ weightTensorDesc.TotalTensorSizeInBytes };
	com_ptr<ID3D12Resource> weightUploadBuffer;
	check_hresult(d3D12Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(weightBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		__uuidof(weightUploadBuffer),
		weightUploadBuffer.put_void()));

	com_ptr<ID3D12Resource> weightBuffer;
	check_hresult(d3D12Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(weightBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		__uuidof(weightBuffer),
		weightBuffer.put_void()));

	// std::wcout << std::fixed; std::wcout.precision(2);
	std::vector<uint16_t> weightElementArray(weightElementCount, f16_val);
	std::vector<FLOAT> weightElementArrayF32(weightElementCount, f32_val);
	void* weightData = g_support_f16 ? (void*)weightElementArray.data() : (void*)weightElementArrayF32.data();
	{
		D3D12_SUBRESOURCE_DATA tensorSubresourceData{};
		tensorSubresourceData.pData = weightData;
		tensorSubresourceData.RowPitch = weightBufferSize;
		tensorSubresourceData.SlicePitch = tensorSubresourceData.RowPitch;

		// Upload the input tensor to the GPU.
		::UpdateSubresources(
			commandList.get(),
			weightBuffer.get(),
			weightUploadBuffer.get(),
			0,
			0,
			1,
			&tensorSubresourceData);

		commandList->ResourceBarrier(
			1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				weightBuffer.get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS
			)
		);
	}

	// bias.
	UINT64 biasBufferSize{ biasTensorDesc.TotalTensorSizeInBytes };
	com_ptr<ID3D12Resource> biasUploadBuffer;
	check_hresult(d3D12Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(biasBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		__uuidof(biasUploadBuffer),
		biasUploadBuffer.put_void()));

	com_ptr<ID3D12Resource> biasBuffer;
	check_hresult(d3D12Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(biasBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		__uuidof(biasBuffer),
		biasBuffer.put_void()));

	std::vector<uint16_t> biasElementArray(biasElementCount, f16_val);
	std::vector<FLOAT> biasElementArrayF32(biasElementCount, f32_val);
	void* biasData = g_support_f16 ? (void*)biasElementArray.data() : (void*)biasElementArrayF32.data();
	{
		D3D12_SUBRESOURCE_DATA tensorSubresourceData{};
		tensorSubresourceData.pData = biasData;
		tensorSubresourceData.RowPitch = biasBufferSize;
		tensorSubresourceData.SlicePitch = tensorSubresourceData.RowPitch;

		// Upload the input tensor to the GPU.
		::UpdateSubresources(
			commandList.get(),
			biasBuffer.get(),
			biasUploadBuffer.get(),
			0,
			0,
			1,
			&tensorSubresourceData);

		commandList->ResourceBarrier(
			1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				biasBuffer.get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS
			)
		);
	}

	DML_BUFFER_BINDING* input_buffers = new DML_BUFFER_BINDING[3];
	// Empty buffer binding.
	input_buffers[0] = { nullptr, 0, 0 };
	input_buffers[1] = { weightBuffer.get(), 0,
						  weightBufferSize };
	input_buffers[2] = { biasBuffer.get(), 0, biasBufferSize };

	DML_BUFFER_ARRAY_BINDING init_buffer_array[1];
	DML_BINDING_DESC init_binding_array[1];
	// Inputs binding desc for initializeing.
	init_buffer_array[0] = { 3, input_buffers };
	init_binding_array[0] = { DML_BINDING_TYPE_BUFFER_ARRAY,
							 &init_buffer_array[0] };
	dmlBindingTable->BindInputs(1, init_binding_array);



	com_ptr<ID3D12Resource> temporaryBuffer;
	if (temporaryResourceSize != 0)
	{
		check_hresult(d3D12Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(temporaryResourceSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			__uuidof(temporaryBuffer),
			temporaryBuffer.put_void()));

		DML_BUFFER_BINDING bufferBinding{ temporaryBuffer.get(), 0, temporaryResourceSize };
		DML_BINDING_DESC bindingDesc{ DML_BINDING_TYPE_BUFFER, &bufferBinding };
		//dmlBindingTable->BindTemporaryResource(&bindingDesc);
	}

	com_ptr<ID3D12Resource> persistentBuffer;
	if (persistentResourceSize != 0)
	{
		check_hresult(d3D12Device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(persistentResourceSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			__uuidof(persistentBuffer),
			persistentBuffer.put_void()));

		// The persistent resource should be bound as the output to the IDMLOperatorInitializer.
		DML_BUFFER_BINDING bufferBinding{ persistentBuffer.get(), 0, persistentResourceSize };
		DML_BINDING_DESC bindingDesc{ DML_BINDING_TYPE_BUFFER, &bufferBinding };
		dmlBindingTable->BindOutputs(1, &bindingDesc);
	}


	// Record execution of the operator initializer.
	dmlCommandRecorder->RecordDispatch(
		commandList.get(),
		dmlOperatorInitializer.get(),
		dmlBindingTable.get());

	// Close the Direct3D 12 command list, and submit it for execution as you would any other command list. You could
	// in principle record the execution into the same command list as the initialization, but you need only to Initialize
	// once, and typically you want to Execute an operator more frequently than that.
	CloseExecuteResetWait(d3D12Device, commandQueue, commandAllocator, commandList);

	// 
	// Bind and execute the operator on the GPU.
	// 
	commandList->SetDescriptorHeaps(ARRAYSIZE(d3D12DescriptorHeaps), d3D12DescriptorHeaps);

	// Reset the binding table to bind for the operator we want to execute (it was previously used to bind for the
	// initializer).

	dmlBindingTableDesc.Dispatchable = dmlCompiledOperator.get();

	check_hresult(dmlBindingTable->Reset(&dmlBindingTableDesc));

	if (temporaryResourceSize != 0)
	{
		DML_BUFFER_BINDING bufferBinding{ temporaryBuffer.get(), 0, temporaryResourceSize };
		DML_BINDING_DESC bindingDesc{ DML_BINDING_TYPE_BUFFER, &bufferBinding };
		dmlBindingTable->BindTemporaryResource(&bindingDesc);
	}

	if (persistentResourceSize != 0)
	{
		DML_BUFFER_BINDING bufferBinding{ persistentBuffer.get(), 0, persistentResourceSize };
		DML_BINDING_DESC bindingDesc{ DML_BINDING_TYPE_BUFFER, &bufferBinding };
		dmlBindingTable->BindPersistentResource(&bindingDesc);
	}

	// Create tensor buffers for upload/input/output/readback of the tensor elements.

	// 24 elements * 4 == 96 bytes.
	UINT64 inputBufferSize{ inputTensorDesc.TotalTensorSizeInBytes };

	com_ptr<ID3D12Resource> uploadBuffer;
	check_hresult(d3D12Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(inputBufferSize),
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		__uuidof(uploadBuffer),
		uploadBuffer.put_void()));

	com_ptr<ID3D12Resource> inputBuffer;
	check_hresult(d3D12Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(inputBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		__uuidof(inputBuffer),
		inputBuffer.put_void()));

	// std::wcout << std::fixed; std::wcout.precision(2);
	std::vector<uint16_t> inputTensorElementArray(inputElementCount, f16_val);
	std::vector<FLOAT> inputTensorElementArrayF32(inputElementCount, f32_val);
	void* inputData = g_support_f16 ? (void*)inputTensorElementArray.data() : (void*)inputTensorElementArrayF32.data();
	{
		D3D12_SUBRESOURCE_DATA tensorSubresourceData{};
		tensorSubresourceData.pData = inputData;
		tensorSubresourceData.RowPitch = inputBufferSize;
		tensorSubresourceData.SlicePitch = tensorSubresourceData.RowPitch;

		// Upload the input tensor to the GPU.
		::UpdateSubresources(
			commandList.get(),
			inputBuffer.get(),
			uploadBuffer.get(),
			0,
			0,
			1,
			&tensorSubresourceData);

		commandList->ResourceBarrier(
			1,
			&CD3DX12_RESOURCE_BARRIER::Transition(
				inputBuffer.get(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_UNORDERED_ACCESS
			)
		);
	}



	DML_BUFFER_BINDING inputBufferBinding[3];
	inputBufferBinding[0] = { inputBuffer.get(), 0, inputBufferSize };
	inputBufferBinding[1] = { weightBuffer.get(), 0, weightBufferSize };
	inputBufferBinding[2] = { biasBuffer.get(), 0, biasBufferSize };
	DML_BINDING_DESC inputBindingDesc[3];
	inputBindingDesc[0] = { DML_BINDING_TYPE_BUFFER, &inputBufferBinding[0] };
	inputBindingDesc[1] = { DML_BINDING_TYPE_NONE, nullptr };
	inputBindingDesc[2] = { DML_BINDING_TYPE_NONE, nullptr };
	dmlBindingTable->BindInputs(3, inputBindingDesc);

	com_ptr<ID3D12Resource> outputBuffer;
	UINT64 outputBufferSize{ outputTensorDesc.TotalTensorSizeInBytes };
	check_hresult(d3D12Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(outputBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		__uuidof(outputBuffer),
		outputBuffer.put_void()));

	DML_BUFFER_BINDING outputBufferBinding{ outputBuffer.get(), 0, outputBufferSize };
	DML_BINDING_DESC outputBindingDesc{ DML_BINDING_TYPE_BUFFER, &outputBufferBinding };
	dmlBindingTable->BindOutputs(1, &outputBindingDesc);

	SYSTEMTIME sys_1;
	GetLocalTime(&sys_1);
	// Record execution of the compiled operator.
	dmlCommandRecorder->RecordDispatch(commandList.get(), dmlCompiledOperator.get(), dmlBindingTable.get());

	CloseExecuteResetWait(d3D12Device, commandQueue, commandAllocator, commandList);

	SYSTEMTIME sys_2;
	GetLocalTime(&sys_2);
	std::wcout << L"Predict time: " << sys_2.wMilliseconds - sys_1.wMilliseconds << " ms\n";

	// The output buffer now contains the result of the identity operator,
	// so read it back if you want the CPU to access it.

	com_ptr<ID3D12Resource> readbackBuffer;
	check_hresult(d3D12Device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(outputBufferSize),
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		__uuidof(readbackBuffer),
		readbackBuffer.put_void()));

	commandList->ResourceBarrier(
		1,
		&CD3DX12_RESOURCE_BARRIER::Transition(
			outputBuffer.get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_COPY_SOURCE
		)
	);

	commandList->CopyResource(readbackBuffer.get(), outputBuffer.get());

	CloseExecuteResetWait(d3D12Device, commandQueue, commandAllocator, commandList);

	D3D12_RANGE tensorBufferRange{ 0, outputBufferSize };
	CHAR* outputBufferData{};
	check_hresult(readbackBuffer->Map(0, &tensorBufferRange, reinterpret_cast<void**>(&outputBufferData)));

	std::wcout << L"Output tensor elements[0-10]: ";
	for (size_t tensorElementIndex{ 0 }; tensorElementIndex < 10; ++tensorElementIndex)
	{
		if (g_support_f16) {
			uint16_t* value = (uint16_t*)outputBufferData;
			std::wcout << *value << L' ';
			outputBufferData += sizeof(uint16_t);
		}
		else {
			FLOAT* value = (FLOAT*)outputBufferData;
			std::wcout << *value << L' ';
			outputBufferData += sizeof(FLOAT);
		}
		
	}
	std::wcout << std::endl;

	D3D12_RANGE emptyRange{ 0, 0 };
	readbackBuffer->Unmap(0, &emptyRange);
}
