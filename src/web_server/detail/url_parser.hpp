#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace miximus::web_server::detail {

/**
 * Simple URL parser that separates path, query parameters, and fragment (anchor)
 * according to RFC 3986 URI specification.
 *
 * The parser operates entirely on string_view for efficiency, assuming the source
 * string remains valid for the lifetime of the parser instance.
 */
class url_parser
{
  public:
    /**
     * Parse a URL resource string into components
     * @param resource The URL resource string (e.g., "/path?param=value#fragment")
     * @return A url_parser instance with parsed components
     */
    static url_parser parse(std::string_view resource);

    /**
     * URL decode a string (static utility function)
     * @param encoded The URL-encoded string
     * @return The decoded string
     */
    static std::string url_decode(std::string_view encoded);

    /**
     * Get the path component without query or fragment
     * @return The path (e.g., "/path" from "/path?param=value#fragment")
     */
    std::string_view get_path() const { return path_; }

    /**
     * Get a specific query parameter value
     * @param name The parameter name
     * @return The parameter value, or empty string_view if not found
     */
    std::string_view get_query_param(std::string_view name) const;

    /**
     * Get the fragment/anchor component
     * @return The fragment (e.g., "fragment" from "/path?param=value#fragment")
     */
    std::string_view get_fragment() const { return fragment_; }

    /**
     * Check if a query parameter exists
     * @param name The parameter name
     * @return true if the parameter exists
     */
    bool has_query_param(std::string_view name) const;

    /**
     * Get all query parameters
     * @return Map of parameter names to values (as string_views)
     */
    const std::unordered_map<std::string_view, std::string_view>& get_query_params() const { return query_params_; }

  private:
    // Private constructor - use static parse() method instead
    explicit url_parser(std::string_view resource);

    void parse_query_string(std::string_view query);

    std::string_view                                       path_;
    std::string_view                                       fragment_;
    std::unordered_map<std::string_view, std::string_view> query_params_;
};

} // namespace miximus::web_server::detail
