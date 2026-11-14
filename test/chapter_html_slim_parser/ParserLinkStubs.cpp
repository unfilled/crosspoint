#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/TokenBoundary.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>

const char* lookupHtmlEntity(const char*, size_t) { return nullptr; }

#include <BidiUtils.h>

bool isExplicitHyphen(uint32_t) { return false; }
bool isSoftHyphen(uint32_t) { return false; }

// Opt-in test hook: DropCapHyphenTest enables deterministic fallback breaks
// (every 5 bytes, inserted hyphen) to exercise the hyphenated layout path.
// Every other test in the suite keeps the historical no-hyphenation stub.
bool gTestHyphenationFallbackEnabled = false;

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string& word, const bool includeFallback) {
  std::vector<Hyphenator::BreakInfo> breaks;
  if (gTestHyphenationFallbackEnabled && includeFallback) {
    for (size_t offset = 5; offset < word.size(); offset += 5) breaks.push_back({offset, true});
  }
  return breaks;
}

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

bool ImageDecoderFactory::isFormatSupported(const std::string& path) {
  return Epub::enableGifProbeForTest && path.ends_with(".gif");
}
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }

void ImageBlock::render(GfxRenderer&, int, int) {}
void ImageBlock::renderPlaceholder(GfxRenderer&, int, int) const {}
bool ImageBlock::needsDecode() const { return false; }
bool ImageBlock::serialize(HalFile&) { return false; }
std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile&) { return nullptr; }
