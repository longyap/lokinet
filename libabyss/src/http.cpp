#include <abyss/http.hpp>
#include <util/str.hpp>

#include <algorithm>

namespace abyss
{
  namespace http
  {
    bool
    HeaderReader::ProcessHeaderLine(std::string_view line, bool& done)
    {
      if (line.size() == 0)
      {
        done = true;
        return true;
      }
      auto idx = line.find_first_of(':');
      if (idx == std::string_view::npos)
        return false;
      std::string_view header = line.substr(0, idx);
      std::string_view val = line.substr(1 + idx);
      // to lowercase
      std::string lowerHeader;
      lowerHeader.reserve(header.size());
      auto itr = header.begin();
      while (itr != header.end())
      {
        lowerHeader += std::tolower(*itr);
        ++itr;
      }
      if (ShouldProcessHeader(lowerHeader))
      {
        val = val.substr(val.find_first_not_of(' '));
        Header.Headers.emplace(std::move(lowerHeader), val);
      }
      return true;
    }
  }  // namespace http
}  // namespace abyss
