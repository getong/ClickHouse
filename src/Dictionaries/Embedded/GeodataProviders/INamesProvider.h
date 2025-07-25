#pragma once

#include <memory>
#include <Dictionaries/Embedded/GeodataProviders/Entries.h>

namespace DB
{

// Iterates over all name entries in data source
class ILanguageRegionsNamesReader
{
public:
    virtual bool readNext(RegionNameEntry & entry) = 0;

    virtual ~ILanguageRegionsNamesReader() = default;
};

using ILanguageRegionsNamesReaderPtr = std::unique_ptr<ILanguageRegionsNamesReader>;


// Regions names data source for one language
class ILanguageRegionsNamesDataSource
{
public:
    // data modified since last createReader invocation
    virtual bool isModified() const = 0;

    // Upper bound on total length of all names
    virtual size_t estimateTotalSize() const = 0;

    virtual ILanguageRegionsNamesReaderPtr createReader() = 0;

    virtual std::string getLanguage() const = 0;

    virtual std::string getSourceName() const = 0;

    virtual ~ILanguageRegionsNamesDataSource() = default;
};

using ILanguageRegionsNamesDataSourcePtr = std::unique_ptr<ILanguageRegionsNamesDataSource>;


// Provides regions names data sources for different languages
class IRegionsNamesDataProvider
{
public:
    /// Returns nullptr if the language data does not exist.
    virtual ILanguageRegionsNamesDataSourcePtr getLanguageRegionsNamesSource(const std::string & language) const = 0;

    virtual ~IRegionsNamesDataProvider() = default;
};

using IRegionsNamesDataProviderPtr = std::unique_ptr<IRegionsNamesDataProvider>;

}
