#include <c10/core/ScalarType.h>
#include <libtorchaudio/sox/types.h>
#include <libtorchaudio/sox/utils.h>
#include <sox.h>

namespace torchaudio::sox {

const std::unordered_set<std::string> UNSUPPORTED_EFFECTS{
    "input",
    "output",
    "spectrogram",
    "noiseprof",
    "noisered",
    "splice"};

void set_seed(const int64_t seed) {
  sox_get_globals()->ranqd1 = static_cast<sox_int32_t>(seed);
}

void set_verbosity(const int64_t verbosity) {
  sox_get_globals()->verbosity = static_cast<unsigned>(verbosity);
}

void set_use_threads(const bool use_threads) {
  sox_get_globals()->use_threads = static_cast<sox_bool>(use_threads);
}

void set_buffer_size(const int64_t buffer_size) {
  sox_get_globals()->bufsiz = static_cast<size_t>(buffer_size);
}

int64_t get_buffer_size() {
  return sox_get_globals()->bufsiz;
}

std::vector<std::vector<std::string>> list_effects() {
  std::vector<std::vector<std::string>> effects;
  for (const sox_effect_fn_t* fns = sox_get_effect_fns(); *fns; ++fns) {
    const sox_effect_handler_t* handler = (*fns)();
    if (handler && handler->name) {
      if (UNSUPPORTED_EFFECTS.find(handler->name) ==
          UNSUPPORTED_EFFECTS.end()) {
        effects.emplace_back(std::vector<std::string>{
            handler->name,
            handler->usage ? std::string(handler->usage) : std::string("")});
      }
    }
  }
  return effects;
}

std::vector<std::string> list_write_formats() {
  std::vector<std::string> formats;
  for (const sox_format_tab_t* fns = sox_get_format_fns(); fns->fn; ++fns) {
    const sox_format_handler_t* handler = fns->fn();
    for (const char* const* names = handler->names; *names; ++names) {
      if (!strchr(*names, '/') && handler->write) {
        formats.emplace_back(*names);
      }
    }
  }
  return formats;
}

std::vector<std::string> list_read_formats() {
  std::vector<std::string> formats;
  for (const sox_format_tab_t* fns = sox_get_format_fns(); fns->fn; ++fns) {
    const sox_format_handler_t* handler = fns->fn();
    for (const char* const* names = handler->names; *names; ++names) {
      if (!strchr(*names, '/') && handler->read) {
        formats.emplace_back(*names);
      }
    }
  }
  return formats;
}

SoxFormat::SoxFormat(sox_format_t* fd) noexcept : fd_(fd) {}
SoxFormat::~SoxFormat() {
  close();
}

sox_format_t* SoxFormat::operator->() const noexcept {
  return fd_;
}
SoxFormat::operator sox_format_t*() const noexcept {
  return fd_;
}

void SoxFormat::close() {
  if (fd_ != nullptr) {
    sox_close(fd_);
    fd_ = nullptr;
  }
}

void validate_input_file(const SoxFormat& sf, const std::string& path) {
  TORCH_CHECK(
      static_cast<sox_format_t*>(sf) != nullptr,
      "Error loading audio file: failed to open file " + path);
  TORCH_CHECK(
      sf->encoding.encoding != SOX_ENCODING_UNKNOWN,
      "Error loading audio file: unknown encoding.");
}

void validate_input_tensor(const torch::Tensor& tensor) {
  TORCH_CHECK(tensor.device().is_cpu(), "Input tensor has to be on CPU.");

  TORCH_CHECK(tensor.ndimension() == 2, "Input tensor has to be 2D.");

  switch (tensor.dtype().toScalarType()) {
    case c10::ScalarType::Byte:
    case c10::ScalarType::Short:
    case c10::ScalarType::Int:
    case c10::ScalarType::Float:
      break;
    default:
      TORCH_CHECK(
          false,
          "Input tensor has to be one of float32, int32, int16 or uint8 type.");
  }
}

caffe2::TypeMeta get_dtype(
    const sox_encoding_t encoding,
    const unsigned precision) {
  const auto dtype = [&]() {
    switch (encoding) {
      case SOX_ENCODING_UNSIGNED: // 8-bit PCM WAV
        return torch::kUInt8;
      case SOX_ENCODING_SIGN2: // 16-bit, 24-bit, or 32-bit PCM WAV
        switch (precision) {
          case 16:
            return torch::kInt16;
          case 24: // Cast 24-bit to 32-bit.
          case 32:
            return torch::kInt32;
          default:
            TORCH_CHECK(
                false,
                "Only 16, 24, and 32 bits are supported for signed PCM.");
        }
      default:
        // default to float32 for the other formats, including
        // 32-bit flaoting-point WAV,
        // MP3,
        // FLAC,
        // VORBIS etc...
        return torch::kFloat32;
    }
  }();
  return c10::scalarTypeToTypeMeta(dtype);
}

torch::Tensor convert_to_tensor(
    sox_sample_t* buffer,
    const int32_t num_samples,
    const int32_t num_channels,
    const caffe2::TypeMeta dtype,
    const bool normalize,
    const bool channels_first) {
  torch::Tensor t;
  uint64_t dummy = 0;
  SOX_SAMPLE_LOCALS;
  if (normalize || dtype == torch::kFloat32) {
    t = torch::empty(
        {num_samples / num_channels, num_channels}, torch::kFloat32);
    auto ptr = t.data_ptr<float_t>();
    for (int32_t i = 0; i < num_samples; ++i) {
      ptr[i] = SOX_SAMPLE_TO_FLOAT_32BIT(buffer[i], dummy);
    }
  } else if (dtype == torch::kInt32) {
    t = torch::from_blob(
            buffer, {num_samples / num_channels, num_channels}, torch::kInt32)
            .clone();
  } else if (dtype == torch::kInt16) {
    t = torch::empty({num_samples / num_channels, num_channels}, torch::kInt16);
    auto ptr = t.data_ptr<int16_t>();
    for (int32_t i = 0; i < num_samples; ++i) {
      ptr[i] = SOX_SAMPLE_TO_SIGNED_16BIT(buffer[i], dummy);
    }
  } else if (dtype == torch::kUInt8) {
    t = torch::empty({num_samples / num_channels, num_channels}, torch::kUInt8);
    auto ptr = t.data_ptr<uint8_t>();
    for (int32_t i = 0; i < num_samples; ++i) {
      ptr[i] = SOX_SAMPLE_TO_UNSIGNED_8BIT(buffer[i], dummy);
    }
  } else {
    TORCH_CHECK(false, "Unsupported dtype: ", dtype);
  }
  if (channels_first) {
    t = t.transpose(1, 0);
  }
  return t.contiguous();
}

const std::string get_filetype(const std::string& path) {
  std::string ext = path.substr(path.find_last_of('.') + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext;
}

namespace {

std::tuple<sox_encoding_t, unsigned> get_save_encoding_for_wav(
    const std::string& format,
    caffe2::TypeMeta dtype,
    const Encoding& encoding,
    const BitDepth& bits_per_sample) {
  switch (encoding) {
    case Encoding::NOT_PROVIDED:
      switch (bits_per_sample) {
        case BitDepth::NOT_PROVIDED:
          switch (dtype.toScalarType()) {
            case c10::ScalarType::Float:
              return std::make_tuple<>(SOX_ENCODING_FLOAT, 32);
            case c10::ScalarType::Int:
              return std::make_tuple<>(SOX_ENCODING_SIGN2, 32);
            case c10::ScalarType::Short:
              return std::make_tuple<>(SOX_ENCODING_SIGN2, 16);
            case c10::ScalarType::Byte:
              return std::make_tuple<>(SOX_ENCODING_UNSIGNED, 8);
            default:
              TORCH_CHECK(false, "Internal Error: Unexpected dtype: ", dtype);
          }
        case BitDepth::B8:
          return std::make_tuple<>(SOX_ENCODING_UNSIGNED, 8);
        default:
          return std::make_tuple<>(
              SOX_ENCODING_SIGN2, static_cast<unsigned>(bits_per_sample));
      }
    case Encoding::PCM_SIGNED:
      switch (bits_per_sample) {
        case BitDepth::NOT_PROVIDED:
          return std::make_tuple<>(SOX_ENCODING_SIGN2, 16);
        case BitDepth::B8:
          TORCH_CHECK(
              false, format, " does not support 8-bit signed PCM encoding.");
        default:
          return std::make_tuple<>(
              SOX_ENCODING_SIGN2, static_cast<unsigned>(bits_per_sample));
      }
    case Encoding::PCM_UNSIGNED:
      switch (bits_per_sample) {
        case BitDepth::NOT_PROVIDED:
        case BitDepth::B8:
          return std::make_tuple<>(SOX_ENCODING_UNSIGNED, 8);
        default:
          TORCH_CHECK(
              false, format, " only supports 8-bit for unsigned PCM encoding.");
      }
    case Encoding::PCM_FLOAT:
      switch (bits_per_sample) {
        case BitDepth::NOT_PROVIDED:
        case BitDepth::B32:
          return std::make_tuple<>(SOX_ENCODING_FLOAT, 32);
        case BitDepth::B64:
          return std::make_tuple<>(SOX_ENCODING_FLOAT, 64);
        default:
          TORCH_CHECK(
              false,
              format,
              " only supports 32-bit or 64-bit for floating-point PCM encoding.");
      }
    case Encoding::ULAW:
      switch (bits_per_sample) {
        case BitDepth::NOT_PROVIDED:
        case BitDepth::B8:
          return std::make_tuple<>(SOX_ENCODING_ULAW, 8);
        default:
          TORCH_CHECK(
              false, format, " only supports 8-bit for mu-law encoding.");
      }
    case Encoding::ALAW:
      switch (bits_per_sample) {
        case BitDepth::NOT_PROVIDED:
        case BitDepth::B8:
          return std::make_tuple<>(SOX_ENCODING_ALAW, 8);
        default:
          TORCH_CHECK(
              false, format, " only supports 8-bit for a-law encoding.");
      }
    default:
      TORCH_CHECK(
          false, format, " does not support encoding: " + to_string(encoding));
  }
}

std::tuple<sox_encoding_t, unsigned> get_save_encoding(
    const std::string& format,
    const caffe2::TypeMeta& dtype,
    const std::optional<std::string>& encoding,
    const std::optional<int64_t>& bits_per_sample) {
  const Format fmt = get_format_from_string(format);
  const Encoding enc = get_encoding_from_option(encoding);
  const BitDepth bps = get_bit_depth_from_option(bits_per_sample);

  switch (fmt) {
    case Format::WAV:
    case Format::AMB:
      return get_save_encoding_for_wav(format, dtype, enc, bps);
    case Format::MP3:
      TORCH_CHECK(
          enc == Encoding::NOT_PROVIDED,
          "mp3 does not support `encoding` option.");
      TORCH_CHECK(
          bps == BitDepth::NOT_PROVIDED,
          "mp3 does not support `bits_per_sample` option.");
      return std::make_tuple<>(SOX_ENCODING_MP3, 16);
    case Format::HTK:
      TORCH_CHECK(
          enc == Encoding::NOT_PROVIDED,
          "htk does not support `encoding` option.");
      TORCH_CHECK(
          bps == BitDepth::NOT_PROVIDED,
          "htk does not support `bits_per_sample` option.");
      return std::make_tuple<>(SOX_ENCODING_SIGN2, 16);
    case Format::VORBIS:
      TORCH_CHECK(
          enc == Encoding::NOT_PROVIDED,
          "vorbis does not support `encoding` option.");
      TORCH_CHECK(
          bps == BitDepth::NOT_PROVIDED,
          "vorbis does not support `bits_per_sample` option.");
      return std::make_tuple<>(SOX_ENCODING_VORBIS, 0);
    case Format::AMR_NB:
      TORCH_CHECK(
          enc == Encoding::NOT_PROVIDED,
          "amr-nb does not support `encoding` option.");
      TORCH_CHECK(
          bps == BitDepth::NOT_PROVIDED,
          "amr-nb does not support `bits_per_sample` option.");
      return std::make_tuple<>(SOX_ENCODING_AMR_NB, 16);
    case Format::FLAC:
      TORCH_CHECK(
          enc == Encoding::NOT_PROVIDED,
          "flac does not support `encoding` option.");
      switch (bps) {
        case BitDepth::B32:
        case BitDepth::B64:
          TORCH_CHECK(
              false, "flac does not support `bits_per_sample` larger than 24.");
        default:
          return std::make_tuple<>(
              SOX_ENCODING_FLAC, static_cast<unsigned>(bps));
      }
    case Format::SPHERE:
      switch (enc) {
        case Encoding::NOT_PROVIDED:
        case Encoding::PCM_SIGNED:
          switch (bps) {
            case BitDepth::NOT_PROVIDED:
              return std::make_tuple<>(SOX_ENCODING_SIGN2, 32);
            default:
              return std::make_tuple<>(
                  SOX_ENCODING_SIGN2, static_cast<unsigned>(bps));
          }
        case Encoding::PCM_UNSIGNED:
          TORCH_CHECK(false, "sph does not support unsigned integer PCM.");
        case Encoding::PCM_FLOAT:
          TORCH_CHECK(false, "sph does not support floating point PCM.");
        case Encoding::ULAW:
          switch (bps) {
            case BitDepth::NOT_PROVIDED:
            case BitDepth::B8:
              return std::make_tuple<>(SOX_ENCODING_ULAW, 8);
            default:
              TORCH_CHECK(
                  false, "sph only supports 8-bit for mu-law encoding.");
          }
        case Encoding::ALAW:
          switch (bps) {
            case BitDepth::NOT_PROVIDED:
            case BitDepth::B8:
              return std::make_tuple<>(SOX_ENCODING_ALAW, 8);
            default:
              return std::make_tuple<>(
                  SOX_ENCODING_ALAW, static_cast<unsigned>(bps));
          }
        default:
          TORCH_CHECK(
              false, "sph does not support encoding: ", encoding.value());
      }
    case Format::GSM:
      TORCH_CHECK(
          enc == Encoding::NOT_PROVIDED,
          "gsm does not support `encoding` option.");
      TORCH_CHECK(
          bps == BitDepth::NOT_PROVIDED,
          "gsm does not support `bits_per_sample` option.");
      return std::make_tuple<>(SOX_ENCODING_GSM, 16);

    default:
      TORCH_CHECK(false, "Unsupported format: " + format);
  }
}

unsigned get_precision(const std::string& filetype, caffe2::TypeMeta dtype) {
  if (filetype == "mp3") {
    return SOX_UNSPEC;
  }
  if (filetype == "flac") {
    return 24;
  }
  if (filetype == "ogg" || filetype == "vorbis") {
    return SOX_UNSPEC;
  }
  if (filetype == "wav" || filetype == "amb") {
    switch (dtype.toScalarType()) {
      case c10::ScalarType::Byte:
        return 8;
      case c10::ScalarType::Short:
        return 16;
      case c10::ScalarType::Int:
        return 32;
      case c10::ScalarType::Float:
        return 32;
      default:
        TORCH_CHECK(false, "Unsupported dtype: ", dtype);
    }
  }
  if (filetype == "sph") {
    return 32;
  }
  if (filetype == "amr-nb") {
    return 16;
  }
  if (filetype == "gsm") {
    return 16;
  }
  if (filetype == "htk") {
    return 16;
  }
  TORCH_CHECK(false, "Unsupported file type: ", filetype);
}

} // namespace

sox_signalinfo_t get_signalinfo(
    const torch::Tensor* waveform,
    const int64_t sample_rate,
    const std::string& filetype,
    const bool channels_first) {
  return sox_signalinfo_t{
      /*rate=*/static_cast<sox_rate_t>(sample_rate),
      /*channels=*/
      static_cast<unsigned>(waveform->size(channels_first ? 0 : 1)),
      /*precision=*/get_precision(filetype, waveform->dtype()),
      /*length=*/static_cast<uint64_t>(waveform->numel()),
      nullptr};
}

sox_encodinginfo_t get_tensor_encodinginfo(caffe2::TypeMeta dtype) {
  sox_encoding_t encoding = [&]() {
    switch (dtype.toScalarType()) {
      case c10::ScalarType::Byte:
        return SOX_ENCODING_UNSIGNED;
      case c10::ScalarType::Short:
        return SOX_ENCODING_SIGN2;
      case c10::ScalarType::Int:
        return SOX_ENCODING_SIGN2;
      case c10::ScalarType::Float:
        return SOX_ENCODING_FLOAT;
      default:
        TORCH_CHECK(false, "Unsupported dtype: ", dtype);
    }
  }();
  unsigned bits_per_sample = [&]() {
    switch (dtype.toScalarType()) {
      case c10::ScalarType::Byte:
        return 8;
      case c10::ScalarType::Short:
        return 16;
      case c10::ScalarType::Int:
        return 32;
      case c10::ScalarType::Float:
        return 32;
      default:
        TORCH_CHECK(false, "Unsupported dtype: ", dtype);
    }
  }();
  return sox_encodinginfo_t{
      /*encoding=*/encoding,
      /*bits_per_sample=*/bits_per_sample,
      /*compression=*/HUGE_VAL,
      /*reverse_bytes=*/sox_option_default,
      /*reverse_nibbles=*/sox_option_default,
      /*reverse_bits=*/sox_option_default,
      /*opposite_endian=*/sox_false};
}

sox_encodinginfo_t get_encodinginfo_for_save(
    const std::string& format,
    const caffe2::TypeMeta& dtype,
    const std::optional<double>& compression,
    const std::optional<std::string>& encoding,
    const std::optional<int64_t>& bits_per_sample) {
  auto enc = get_save_encoding(format, dtype, encoding, bits_per_sample);
  return sox_encodinginfo_t{
      /*encoding=*/std::get<0>(enc),
      /*bits_per_sample=*/std::get<1>(enc),
      /*compression=*/compression.value_or(HUGE_VAL),
      /*reverse_bytes=*/sox_option_default,
      /*reverse_nibbles=*/sox_option_default,
      /*reverse_bits=*/sox_option_default,
      /*opposite_endian=*/sox_false};
}

} // namespace torchaudio::sox
