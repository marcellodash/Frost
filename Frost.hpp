// We need the ClassPath's streams
#include "ClassPath/include/Streams/Streams.hpp"
// We need files too
#include "ClassPath/include/File/File.hpp"
// We need ClassPath database too
#include "ClassPath/include/Database/SQLFormat.hpp"
// We need strings
#include "ClassPath/include/Strings/Strings.hpp"
// And hash tables too
#include "ClassPath/include/Hash/HashTable.hpp"
// We need crypto code too for the key stuff
#include "ClassPath/include/Crypto/OpenSSLWrap.hpp"

// We need database model declaration (we are using SQLite here) too
#include "ClassPath/include/Database/Database.hpp"
// We need database querys too
#include "ClassPath/include/Database/Query.hpp"

#if KeepPreviousVersionFormat == 1
#define DEFAULT_INDEX	  "__index.db"
#define PROTOCOL_VERSION  "1.0"
#else
  #define DEFAULT_INDEX	  "index.frost"
  #define PROTOCOL_VERSION  "2.0"
#endif

// Forward declare some class we'll be using to avoid including a lot of stuff in the header
namespace File { class Chunk; class MultiChunk; }


namespace Frost
{
    /** The kind of memory block we are using */
    typedef Utils::MemoryBlock MemoryBlock;
    /** The kind of String class we are using too */
    typedef Strings::FastString String;

#if KeepPreviousVersionFormat == 1
    /** For easier access to complex SQL queries */
    typedef Database::Query::Select Select;
    typedef Database::Query::UnsafeRowIterator RowIterT;
    typedef Database::Query::Delete Delete;
    typedef Database::Query::CreateTempTable CreateTempTable;
#endif


    /** This class is used to build sessions keys out of the given user private key.

        The symmetric mode of encryption is used in CTR block mode.
        where the nonce is derived from the SHA256(Multichunk) ^ counter

        The key used for the encryption is derived from the asymmetric encryption
        algorithm.

        It's updated at pseudo-regular interval, and synchronization point are used
        to figure out if the next block is a salt used to update the key or a ciphertext.
        Typically, a key is build like this:
        @verbatim
            // random Salt (256 bits) is generated, || means concatenation.
            key = KDF(Salt || MasterKey)

            cipheredChunk = Salt
            for each encryption block in the multichunk:
               nonce = SHA256(Multichunk) ^ counter
               cipheredChunk = cipheredChunk || AES_CTR_enc(key, Multichunk, nonce)
               // The code below is not implemented yet
               // if length(cipheredData) > securityThreshold:
               //    create new key (see above)
               //    cipheredChunk = cipheredChunk || newSalt
        @endverbatim

        For decrypting, the algorithm run in reverse:
        @verbatim
            Salt = ciphertext[0..256 bits]
            key = KDF(Salt || MasterKey)
            for each encrypted block in the multichunk:
                nonce = SHA256(Multichunk) ^ counter
                clearText = AES_CTR_dec(key, cipherText[0..256], nonce)
        @endverbatim
    */
    class KeyFactory
    {
        // Type definition and enumerations
    public:
        /** The cryptographic primitive we use for asymmetric encrypting */
        typedef Crypto::OSSL_ECIES<NID_secp224k1>   AsymmetricT;
        /** The cryptographic primitive we use for symmetric encrypting */
        typedef Crypto::OSSL_AES                    SymmetricT;
        /** The cryptographic primitive we use for large dataset hashing */
        typedef Crypto::OSSL_SHA256                 BigHashT;

        /** The private key for asymmetric */
        typedef AsymmetricT::PrivateKey             AsymPrivKeyT;
        /** The public key for asymmetric */
        typedef AsymmetricT::PublicKey              AsymPubKeyT;

        /** The Key derivation function to use */
        typedef Hashing::KDF1<256, 256, BigHashT>   KeyDerivFuncT;
        /** The Key derivation function to use for password */
        typedef Hashing::PBKDF1<256, 256, BigHashT> PWKeyDerivFuncT;


        /** The master symmetric key */
        typedef uint8 KeyT[BigHashT::DigestSize];

        // Members
    private:
        /** The master key that's used while the system is running */
        KeyT         masterKey;
        /** The current salt */
        KeyT         salt;

        /** The current counter */
        uint32       counter;
        /** The current opaque nonce */
        KeyT         hashChunkNonce;

        // Interface
    public:
        /** Load the session key out of the given key vault
            @param fileVault       A path to the file vault to open.
            @param cipherMasterKey The ciphered master key that's read from the index database.
            @param password        The password used to protect the private key
            @return empty string on success, or the error message on failure.
                    if the file does not exist,
                    or if the mode does not fit the expected mode (0600)
                    or if the file format does not work */
        String loadPrivateKey(const String & fileVault, const MemoryBlock & cipherMasterKey, const String & password = "", const String & ID = "");
        /** Create file vault if it does not exist, store the new created private key,
            protected by the given password, and generate a master key to be used for
            this session.
            @param cipherMasterKey On output, contains the master key that was encrypted with the
                                   generated private key. This should be saved in the index database.
            @param fileVault       A path to the file vault to create.
            @param password        The password to use to protect the private key
            @return empty string on success, or the error message on failure.
            @return false if the file does already exist
                    (complete directory hierarchy will be created for this file) */
        String createMasterKeyForFileVault(MemoryBlock & cipherMasterKey, const String & fileVault, const String & password = "", const String & ID = "");

        /** Increment the counter and get the current key.
            This must be called before any 'AES_CTR()' call in the algorithm described above */
        void incrementNonce(KeyT & keyOut)
        {
            counter++;
            // Check if all parameters are aligned, and if so, process 4 by 4
            if (((uint64)(&keyOut[0]) & 0x3) == 0)
            {
                for (uint32 i = 0; i < ArrSz(salt); i+= sizeof(counter))
                    *(uint32*)&keyOut[i] = *(uint32*)&hashChunkNonce[i] ^ counter;
                return;
            }
            // Avoid unaligned memory access per loop turn
            uint8 cnt[4] = { (uint8)(counter >> 24), (uint8)((counter >> 16) & 0xFF), (uint8)((counter >> 8) & 0xFF), (uint8)(counter & 0xFF) };
            for (uint32 i = 0; i < ArrSz(salt); i+= sizeof(counter))
            {
                keyOut[i+0] = hashChunkNonce[i+0] ^ cnt[0];
                keyOut[i+1] = hashChunkNonce[i+1] ^ cnt[1];
                keyOut[i+2] = hashChunkNonce[i+2] ^ cnt[2];
                keyOut[i+3] = hashChunkNonce[i+3] ^ cnt[3];
            }
        }
        /** Create a new nonce and reset the counter.
            This creates the 'nonce' in the algorithm described above */
        void createNewNonce(const KeyT & hash) { counter = 0; memcpy(hashChunkNonce, hash, sizeof(hash)); }

        /** Create a new key (and a salt).
            This creates the 'key' in the algorithm described above */
        void createNewKey(KeyT & keyOut)
        {
            Random::fillBlock(salt, ArrSz(salt));
            // Hash the random block to prevent state guessing attacks so no data in output comes from the random output directly.
            BigHashT hash; hash.Start(); hash.Hash(salt, ArrSz(salt)); hash.Finalize(salt);
            deriveNewKey(keyOut);
        }

        /** Get the salt */
        void getCurrentSalt(KeyT & outSalt) const { memcpy(outSalt, salt, ArrSz(salt)); }


        /** Set the current salt (extracted from the ciphertext) */
        void setCurrentSalt(const KeyT & inSalt) { memcpy(salt, inSalt, ArrSz(salt)); }
        /** Derive the key out of the current salt */
        void deriveNewKey(KeyT & keyOut) const
        {
            KeyDerivFuncT kdf;
            kdf.Hash(masterKey, ArrSz(masterKey));
            kdf.finalizeWithExtraInfo(keyOut, salt, (uint32)ArrSz(salt));
        }
    };

    /** Get the key factory singleton */
    KeyFactory & getKeyFactory() { static KeyFactory factory; return factory; }

    /** The progress callback that's called regularly by the backup / restoring process */
    struct ProgressCallback
    {
        enum Action { Backup = 0, Restore, Purge };
        enum FlushMode  { FlushLine = 0, KeepLine, EraseLine };
        String getActionName(const Action action) { const char * actions[] = {"Backup", "Restore", "Purge"}; return actions[action]; }

        /** This method is called while an operation is running.
            The protocol for the sizeDone, totalSize, index and count is as follow:
              1) Each time a new entry is processed, index must be changed (likely increased). The sizeDone value is set to 0, and the totalSize is set to non-zero value.
                 The callback should not validate the line here if it output on the console/database
              2) While the entry is processed, the currentFilename will not change, the index will not change, but the sizeDone and totalSize will likely change.
                 The callback should still not validate the line here
              3) When the entry is done processing, sizeDone and totalSize will be equal.
                 The callback can validate the line here.
              If all sizeDone, totalSize, index, and count are zero, then the line is not validated, action is ignored, only the currentFilename is revelant for output.
            @return false to interrupt the process */
        virtual bool progressed(const Action action, const String & currentFilename, const uint64 sizeDone, const uint64 totalSize, const uint32 index, const uint32 count, const FlushMode mode) = 0;
        /** This method is called when the processing must warn the user.
            @return false to interrupt the process */
        virtual bool warn(const Action action, const String & currentFilename, const String & message, const uint32 sourceLine = 0) { return true; }
        #define WARN_CB(action, file, msg) callback.warn(action, file, msg, __LINE__)
        virtual ~ProgressCallback() {}
    };

    /** The purge strategy.
        @sa purgeBackup */
    enum PurgeStrategy
    {
        Fast            = 1,      //!< The fast strategy for pruning old backup, that's not space efficient.
        FindLostChunk   = Fast,   //!< Find lost chunk and remove them from the database index. If multichunk contains only garbage collected chunks, it's deleted.
        Slow            = 2,      //!< The slow strategy optimize for space, but it's not compute efficient.
        MergeMultiChunk = Slow,   //!< Find lost chunk and remove them from the database index. Recreate complete multichunk out of the remaining one,
                                  //!< downloading them, removing the useless chunk from them, and uploading complete multichunk again.
    };

    /** The overwrite strategy.
        @sa restoreBackup */
    enum OverwritePolicy
    {
        No          = 0, //!< No overwrite, nor deletion allowed
        Yes         = 1, //!< Overwrite and deletion allowed
        Update      = 2, //!< Overwrite allowed if the new item is newer than the one on the filesystem
    };

    /** Some useful methods to convert between internal checksum to hexdecimal */
    namespace Helpers
    {
        /** The allowed compressors */
        enum CompressorToUse
        {
            None = 0,    //!< No compression done to this multichunk
            ZLib = 1,    //!< Using ZLib compression
            BSC  = 2,    //!< Using BSC compression
            // No other compressor supported

            Default = -1,  //!< When not specified, this will select the global 'compressor' value
        };
        /** Convert a small binary blob to a string. */
        String fromBinary(const uint8 * data, const uint32 size, const bool base85 = true);
        /** Convert a string back to the binary blob. */
        bool toBinary(const String & src, uint8 * data, uint32 & size, const bool base85 = true);
        /** Encrypt a given block with AES counter mode.
            @warning Beware that this use the current key factory to figure out the current key and nonce */
        bool AESCounterEncrypt(const KeyFactory::KeyT & nonceRandom, const ::Stream::InputStream & input, ::Stream::OutputStream & output);
        /** Decrypt a given block with AES counter mode.
            @warning Beware that this use the current key factory to figure out the current key and nonce */
        bool AESCounterDecrypt(const KeyFactory::KeyT & nonceRandom, const ::Stream::InputStream & input, ::Stream::OutputStream & output);

        /** Close a currently filled multichunk and save in database and filesystem */
//        bool closeMultiChunk(const String & basePath, File::MultiChunk & multiChunk, uint64 multichunkListID, uint64 * totalOutSize, ProgressCallback & callback, uint64 & previousMultichunkID, CompressorToUse actualComp = Default);
        /** Extract a chunk out of a multichunk */
        File::Chunk * extractChunk(String & error, const String & basePath, const String & MultiChunkPath, const uint64 MultiChunkID, const size_t chunkOffset, const String & chunkChecksum, const String & filterMode, File::MultiChunk * cache, ProgressCallback & callback);
        /** Allocate a chunk list ID */
        unsigned int allocateChunkList();
    }

    /** The archive format used in Frost v2.
        Using a database for index seems like a good idea, but it does not scale very well when the number of entries
        is large, even when the Database's Index are created correctly.
        This is because the database file itself is becoming large and a lot of swapping is done to load pages containing the indexes.

        During both backup and restore process, the index file is not mutated, leading to only increase in size (backup only).
        During purging, the file is mutated. In order to guaranty atomicity of purge-slow process with any data loss, the index file
        is rewritten to a new file, and only when this file is completed, it's swapped with the previous file which is then deleted

        If you are synchronizing the index file remotely, then this means that slow-purging will require sending a whole new file again,
        while using the default system only requires sending the modified part (with rsync, it's done automatically)

        So, in order to speed up our backup process, I'm changing the system from DB to a binary format.
        In order to understand this design choice, on a medium sized dataset (250GB), the number of Chunk is around 12e6
        If each chunk is 20 bytes (SHA-1) + size (2 bytes) + ID (4 bytes) then this takes 312MB in memory (or on a file)
        When backing up, restoring and purging, a fast O(log N) access is required to this array, and this imposes that the chunk cache
        stays in memory else it will be too slow.

        Currently, because of the "append"-like structure we'd like to keep for the index, we have to consolidate multiple chunk cache
        fragments into a large one. Because a single chunk is never stored twice, we don't need to write again the complete cache
        to the index file, as long as we write the new items from the cache in a new fragment.
        This means that we'll have to hold two versions in memory while backing up and purging, the complete cache, and the "new items"
        cache

        Concerning multichunks, the same process applies, but since the number of multichunk is much lower, it's not an algorithm issue.
        We also store chunk lists fragments (an array of chunk's ID) identified by a UID for each multichunk, and for each file.

        A typical backup folder is around 100k files, the chunk list's UID is 4 bytes, so it's negligible

        The file tree is also stored as an array, but unlike the database format, we store the complete tree for each revision to avoid
        having to consolidate the file tree
        So, for a backup tree of 100k files, each file consumes:
          - 4 bytes for UID
          - 4 bytes for parent UID
          - 4 bytes (chunk lists' UID)
          - ~60 bytes (around for file's metadata)
          - ~22 bytes (for file's base name)
          = 94 bytes per file on average => 9.4MB of data for 100k files
        Because each file's record is variable size, and we want to avoid storing useless "length" field.
        Metadata size is known while parsing starts on the metadata field, so this does not requires any length storage.
        File's name is zero-char terminated. The last entry in a File tree block might get padded with zero so it ends on a 32 bits boundary


        Like many archive format, we except being able to seek anywhere in the archive (unlike tar, but more like zip) as O(1), so here's
        the idea for implementation:

        # FILE HEADER
          The file header will contain the magic number ('Frst' or 0x46727374), followed by a 32 bit version/state (currently 0x2)
          A 32 bit offset to the main catalog of object, in 4-bytes unit (this limits the index' size to 16GB maximum)
          The ciphered's master key follows (108 bytes).
          If the catalog offset is 0, then a full catalog is expected to be found at end of file. This shortcut allows no modification
          to the file header on backup, and only have the index file to grow.

        # DATA HEADER
          Each data block starts by a header containing 3 bits : data block type, followed by 29 bits : data block size in 4-bytes units
          See below for a description of the possible data block type
          All data block will be aligned on a 32 bits boundary in the file

        # CATALOG
          The catalog is a special data block object with type 'B' that contains the offset of the other (main/valid) data block in the file.
          Since no modification to the index is done during backup (except for adding data to the index), this is the only block that can be
          mutated (but this is not required, and the default implementation does not do it)
          The catalog contains a reference to the previous version of the catalog (from a previous run of backup), and the position of the
          current file tree object, plus the position to the chunk and multichunk indexes index block

        Data blocks

        # Chunks blocks - type 'C'
          Chunks blocks also have an additional 32 bits header that stores the revision they appeared in
          Chunks are indexed by their checksum. Since index are required for all operations (backup, restoring and purging), a
          consolidation step is done upon starting Frost that's merging all the index blocks. This means that the current array that maps
          chunk's checksum is rebuilt at Frost starting time.
          For backup, there is only one Chunk block per revision (the "difference new" block is directly dumped to the file via mmap and memcpy)
          When purging fast, the file tree starting at the kept revision is analyzed to find out if it needs chunks from the previous revisions
          If it does, then a new chunk block is created with the kept revision - 1 and the chunks are copied to these blocks.
          Then the previous chunks are punched (that is, for system supporting punch hole system call), so they don't use file space anymore.
          The end user results is that after fast purging, the index file size actually increase but size on storage actually decrease (via the
          punch hole feature)
          Useless multichunks are deleted too, but it's not part of this file format process


        # Chunk lists blocks - type 'L'
          Chunk lists block have a 32 bits header containing the chunks list's UID.
          It's then followed by a linear array of chunk id. Offset are not saved because the array is kept in order
          There is one chunk list per file, one per multichunk.

        # Multichunks - type 'M'
          Multichunks have a 16 bits header that stores the multichunk ID (only 65536 multichunk are being used per backup)
          Then follows their linked chunk list ID (32 bits), the filter argument conditions (16 bits for the filter argument's index
          in the filter arguments block).
          The last field is a 32 bytes string holding the SHA256 of the multichunk (this is used to find the multichunk in the backup directory)

        # Filter argument list block - type 'A'
          A zero-byte terminated string array (delimited by '\n') containing the list of filter arguments

        # File tree block - type 'F'
          A file tree block has a 32 bits header that stores the revision number for this file tree.
          Then follows a linked list of file tree item (see above) (around 100 bytes per file). Each item is separated by a zero byte
          after the file's base name.

        # Metadata block entry - type 'M'
          There's usually only one such block in the file.
          This block is there for user based information, such as description storage of the backup set, the initial backup path
          Additional information is possible here

        # Catalog block entry - type 'B'
          A catalog block entry has a 32 bits header containing the revision number for backup.
          A 32 bits offset (in 4 bytes unit) follow that stores the offset to the previous revision's catalog position
          It then stores the offset to the current's revision's chunks block as a 32 bits number
          The offset to the multichunks follow (as a 32 bits number)
          Then the offset to the current revision file tree follows (as a 32 bits number)
          Optionally, the offset to the filter argument list block can follow (as a 32 bits number),
          followed by offset to the metadata block entry (as a 32 bits number)

        @note Archive integers are stored as native endianness (typically little-endian) so they can be memory mapped directly.
              This means that you can not backup a system with a given endianness and restore on a system with a different endianness.
              This is usually not required
    */
    namespace FileFormat
    {
        /** Check if a memory block is zero */
        template <size_t N>
        inline bool isZero(const uint8 (&a)[N]) { for (size_t i = 0; i < N; i++) if (a[i]) return false; return true; }


        // Starting from now, everything is typically packed
#pragma pack(push, 1)
        /** The file's offset used */
        struct Offset
        {
            uint32 offset;
            /** Helper function to convert from file storage to real offset */
            inline uint64 fileOffset() const { return offset * 4ULL; };
            /** Helper function to convert from real offset to file storage */
            inline void fileOffset(const uint64 off) { Assert((off & 3) == 0 && "Offset must be a aligned on 4 bytes"); offset = (uint32)(off >> 2ULL); }

            /** Default construction */
            Offset(uint64 offset = 0) { fileOffset(offset); }
        };


        /** The data block header */
        struct DataHeader
        {
            union {
                struct {
                    /** The data type (check the Type enumeration for the actual meaning) */
                    uint32  type : 3;
                    /** The block size in 4 bits unit (up to 2GB) */
                    uint32  blockSize : 29;
                };
                /** The data block type and size */
                uint32 typeAndSize;
            };
            /** The possible block type */
            enum Type
            {
                Catalog             =   0, //!< A Catalog block type
                Chunk               =   1, //!< The chunks block
                ChunkList           =   2, //!< The list of chunks
                Multichunk          =   3, //!< The multichunks block
                FilterArgument      =   4, //!< The filters argument list
                FileTree            =   5, //!< The file tree
                Metadata            =   6, //!< A metadata block
                Extended            =   7, //!< Extended block type (the next extra word contains the type)
            };
            /** Check if this header is correct */
            bool isCorrect(const int64 fileSize, const uint64 fileOffset) const { return blockSize * 4 + fileOffset <= fileSize; }
            /** Get the size for this block (not the header size, use sizeof(DataHeader) to get it) */
            uint64 getSize() const { return blockSize * 4; }
            /** Set the size for this block (not the header size) obviously */
            void setSize(const uint64 s) { blockSize = (s+3)/4; }
            /** Dump the header (for debugging purpose only) */
            void dump()
            {
                /** The type to name macro */
                static const char * TypeToName[] = { "Catalog", "Chunk", "ChunkList", "Multichunk", "FilterArgument", "FileTree", "Metadata", "Extended" };
                fprintf(stdout, "[t:%s,s:%llu]", TypeToName[type], getSize());
            }

            /** Default construction */
            DataHeader(Type type = Catalog, const uint32 size = 0) : type(type), blockSize(size) {}
        };

        /** The catalog data block */
        struct Catalog
        {
            /** The block header */
            DataHeader header;
            /** The revision number */
            uint32 revision;
            /** The revision's time (in seconds since Epoch) */
            uint32 time;
            /** Previous catalog offset */
            Offset previous;
            /** The chunks's block offset */
            Offset chunks;
            /** The chunk lists' chained list offset */
            Offset chunkLists;
            /** The number of chunk lists at the given offset (they follow each other) */
            uint32 chunkListsCount;
            /** The multichunks block offset */
            Offset multichunks;
            /** The number of multichunks at the given offset (they follow each other) */
            uint32 multichunksCount;
            /** The file tree block offset */
            Offset fileTree;
            /** Optional filter argument list */
            Offset optionFilterArg;
            /** Optional metadata block */
            Offset optionMetadata;

            /** Check if this block is correct */
            bool isCorrect(const int64 fileSize, const uint64 fileOffset) const
            {
                return header.isCorrect(fileSize, fileOffset)  // Does header fit ?
                    && (fileSize >= fileOffset + getSize())                // Do we completely fit without optional fields ?
                    && (previous.fileOffset() <= fileSize)                      // Does the previous block offset fit ?
                    && (chunks.fileOffset() <= fileSize)                        // Does the chunks block offset fit ?
                    && (chunkLists.fileOffset() <= fileSize)                    // Do the chunk lists fit ?
                    && (multichunks.fileOffset() <= fileSize)                   // Does the multichunk block offset fit ?
                    && (fileTree.fileOffset() <= fileSize)                      // Does the file tree offset fit ?
                    && (optionFilterArg.fileOffset() <= fileSize)               // Does the optional filter argument fit ?
                    && (optionMetadata.fileOffset() <= fileSize);               // Does the optional metadata offset fit ?
            }
            /** Get the block expected size */
            static uint64 getSize() { return sizeof(Catalog); }
            /** Load the structure from the given memory pointer */
            bool load(const uint8 * ptr, const uint64 size)
            {
                if (size <= sizeof(header)) return false; // Check if we can read the header
                memcpy(this, ptr, sizeof(header));
                if (size < header.getSize()) return false; // Check if the size is coherent with what we expect
                memcpy(this, ptr, header.getSize());
                return true;
            }
            /** Write the structure to the given memory pointer */
            void write(uint8 * ptr) { header.setSize(getSize()); memcpy(ptr, this, header.getSize()); }
            /** Dump the catalog offset (for debugging purpose) */
            void dump()
            {
                header.dump();
                fprintf(stdout, " Catalog rev%u %s\n", revision, (const char*)Time::Time(time).toDate());
                fprintf(stdout, " Off prev: %llu\n", previous.fileOffset());
                fprintf(stdout, " Off chunk: %llu\n", chunks.fileOffset());
                fprintf(stdout, " Off chunklist: %llu (%u lists)\n", chunkLists.fileOffset(), chunkListsCount);
                fprintf(stdout, " Off mchunk: %llu (%u mchunks)\n", multichunks.fileOffset(), multichunksCount);
                fprintf(stdout, " Off filetree: %llu\n", fileTree.fileOffset());
                fprintf(stdout, " Off filterArg: %llu\n", optionFilterArg.fileOffset());
                fprintf(stdout, " Off metadata: %llu\n", optionMetadata.fileOffset());
            }

            /** Default constructor */
            Catalog(const uint32 revision = 0)
                : header(DataHeader::Catalog), revision(revision), time(::Time::Time::Now().Second()), chunkListsCount(0), multichunksCount(0) {}
        };


        /** The internal chunk item */
        struct Chunk
        {
            /** The chunk's checksum */
            uint8 checksum[20];
            /** The chunk's size */
            uint16 size;
            /** The multichunk that's holding this chunk (up to 65536 possible multichunk) */
            uint16 multichunkID;
            /** The chunk unique identifier */
            uint32 UID;

            // These operators are required for sorting them
            /** If we have an UID, use that for comparing fast, else use the checksum for this */
            bool operator == (const Chunk & k) const { return UID && k.UID ? UID == k.UID : size == k.size && memcmp(checksum, k.checksum, ArrSz(checksum)) == 0; }
            bool operator <= (const Chunk & k) const { return (size < k.size) || (size == k.size && memcmp(checksum, k.checksum, ArrSz(checksum)) <= 0); }
            bool operator != (const Chunk & k) const { return !(*this == k); }

            /** This is required for the array's sorting functions (we don't care about UID here) */
            static int compareData(const Chunk & a, const Chunk & b) { if (a.size < b.size) return -1; if (a.size > b.size) return 1; return memcmp(a.checksum, b.checksum, ArrSz(a.checksum)); }

            Chunk(const uint32 UID = 0) : UID(UID), size(0), multichunkID(0) { memset(this, 0, sizeof(Chunk) - sizeof(UID));  }
            Chunk(const uint8 chksum[20], const uint16 size) : size(size), multichunkID(0), UID(0) { memcpy(checksum, chksum, 20); }
        };

        /** This is used to search and sort the chunk array by UID for restoring, purging mainly */
        struct ChunkUIDSorter
        {
            /** This is required for the array's sorting functions (we don't care about UID here) */
            static int compareData(const Chunk & a, const Chunk & b) { return a.UID < b.UID ? -1 : (a.UID == b.UID ? 0 : 1); }
        };


        /** The chunks block */
        struct Chunks
        {
            /** The block header */
            DataHeader header;
            /** The chunk array revision */
            uint32     revision;
            /** The sorted array of chunk's structure */
            Container::PlainOldData<Chunk>::Array chunks;
            /** (internal) Is the list mapped - readonly */
            bool mapped;

            /** Check correctness of this information for testing purpose */
            bool isCorrect(const uint64 fileSize, const uint64 fileOffset = 0) const { return header.isCorrect(fileSize, fileOffset) && fileSize >= getSize() + fileOffset; }
            /** Get the structure size (as some have optional fields) */
            uint64 getSize() const { return sizeof(header) + sizeof(revision) + chunks.getSize() * sizeof(Chunk); }
            /** Load the structure from the given memory pointer */
            bool load(const uint8 * ptr, const uint64 size)
            {

                if (size <= sizeof(header) + sizeof(revision)) return false; // Check if we can read the header
                memcpy(this, ptr, sizeof(header)+sizeof(revision));
                size_t elemCount = (header.getSize() - sizeof(header) - sizeof(revision)) / sizeof(Chunk);
                if (size < elemCount) return false;
                // Then read all (remaining chunks)
                if (mapped) (void)chunks.getMovable(); // If mapped from a readonly area, release it
                else        chunks.Clear();
                chunks.Grow(elemCount, (Chunk*)(ptr + sizeof(header) + sizeof(revision)));
                return true;
            }
            /** Load from a read-only memory without making a copy of the memory area.
                @warning This array after this call can not be modified as it points to fixed memory it has not allocated
                @warning You must call this method again with 0 for the pointer to tell the system to release the array without actually freeing the area */
            bool loadReadOnly(const uint8 * ptr, const uint64 size)
            {
                if (ptr == 0) { (void)chunks.getMovable(); return true; }
                if (size <= sizeof(header) + sizeof(revision)) return false; // Check if we can read the header
                memcpy(this, ptr, sizeof(header)+sizeof(revision));
                size_t elemCount = (header.getSize() - sizeof(header) - sizeof(revision)) / sizeof(Chunk);
                if (size < header.getSize()) return false;
                // Capture the pointer
                Container::PlainOldData<Chunk>::Array::Internal intern = { (Chunk*)(ptr + sizeof(header)+sizeof(revision)), elemCount, elemCount };
                chunks.Clear(); // Avoid any leak
                // Rebuild the array with the captured data
                new (&chunks) Container::PlainOldData<Chunk>::Array(intern);
                mapped = true;
                return true;
            }
            /** Write the structure to the given memory pointer */
            void write(uint8 * ptr) { if (mapped) return; header.setSize(getSize()); memcpy(ptr, this, sizeof(header) + sizeof(revision)); memcpy(ptr + sizeof(header) + sizeof(revision), &chunks.getElementAtUncheckedPosition(0), chunks.getSize() * sizeof(Chunk)); }
            /** Clear the array */
            inline void Clear() { if (mapped) (void)chunks.getMovable(); else chunks.Clear(); }
            /** Find a chunk ID from its checksum (return -1 if not found) */
            uint32 findChunk(Chunk & chunk) const { size_t pos = chunks.indexOfSorted(chunk, 0); return pos == chunks.getSize() ? (uint32)-1 : chunks[pos].UID; }

            Chunks(const uint32 revision = 0) : header(DataHeader::Chunk), revision(revision), mapped(false) {}
            ~Chunks() { if (mapped) (void)chunks.getMovable(); }
        };

        /** The chunk list block */
        struct ChunkList
        {
            /** The block header */
            DataHeader  header;
            union
            {
                struct
                {
                    /** The list unique identifier */
                    uint32      UID : 31;
                    /** If the offset field is present in this object */
                    uint32      offset : 1;
                };
                uint32 _h; // This is for sizeof
            };
            /** The array of chunks UID */
            Container::PlainOldData<uint32>::Array chunksID;
            /** The array of offset in the chunk (this is used to have O(1) search in multichunks instead of O(N)) */
            Container::PlainOldData<uint32>::Array offsets;


            /** Check correctness of this information for testing purpose */
            bool isCorrect(const uint64 fileSize, const uint64 fileOffset = 0) const { return header.isCorrect(fileSize, fileOffset) && fileSize >= getSize() + fileOffset; }
            /** Get the structure size (as some have optional fields) */
            uint64 getSize() const { return sizeof(header) + sizeof(_h) + chunksID.getSize() * sizeof(uint32) + offsets.getSize() * sizeof(uint32); }
            /** Load the structure from the given memory pointer */
            bool load(const uint8 * ptr, const uint64 size)
            {
                if (size <= sizeof(header) + sizeof(_h)) return false; // Check if we can read the header
                memcpy(this, ptr, sizeof(header)+sizeof(_h));

                size_t elemCount = offset ? (header.getSize() - sizeof(header) - sizeof(_h)) / (2*sizeof(uint32)) : (header.getSize() - sizeof(header) - sizeof(_h)) / sizeof(uint32);
                if (size < header.getSize()) return false;
                // Then read all (remaining chunks)
                chunksID.Clear();
                chunksID.Grow(elemCount, (uint32*)(ptr + sizeof(header) + sizeof(_h)));
                offsets.Clear();
                if (offset) offsets.Grow(elemCount, (uint32*)(ptr + sizeof(header) + sizeof(_h) + elemCount * sizeof(uint32)));
                return true;
            }
            /** Append a chunk ID and optional offset to the list */
            void appendChunk(const uint32 ID, const uint32 off = 0)
            {
                chunksID.Append(ID);
                if (offset) offsets.Append(off);
            }
            /** Write the structure to the given memory pointer */
            void write(uint8 * ptr)
            {
                header.setSize(getSize());
                memcpy(ptr, this, sizeof(header) + sizeof(_h));
                memcpy(ptr + sizeof(header) + sizeof(_h), &chunksID.getElementAtUncheckedPosition(0), chunksID.getSize() * sizeof(uint32));
                memcpy(ptr + sizeof(header) + sizeof(_h) + chunksID.getSize() * sizeof(uint32), &offsets.getElementAtUncheckedPosition(0), offsets.getSize() * sizeof(uint32));
            }
            /** Find the offset for the given chunk UID (only works for list with offset obviously).
                This performs a O(N) search on the index here */
            size_t getChunkOffset(const uint32 chunkID) const
            {
                 if (offset)
                 {
                     size_t pos = chunksID.indexOf(chunkID);
                     if (pos != chunksID.getSize()) return (size_t)offsets.getElementAtPosition(pos);
                 }
                 return (size_t)-1;
            }

            ChunkList(const uint32 UID = 0, const bool withOffset = false) : header(DataHeader::ChunkList), UID(UID), offset(withOffset ? 1 : 0) {}
        };
        /** The Chunk Lists (it's a hash table of ChunkList which follow each other in the file). The size of this array is stored in the catalog */
        typedef Container::HashTable<ChunkList, uint32> ChunkLists;

        /** Multichunks block. */
        struct Multichunk
        {
            /** The block header */
            DataHeader  header;
            /** The chunk list ID used */
            uint32      listID;
            /** The multichunk unique identifier */
            uint16      UID;
            /** The filter argument's index in the filter argument's object */
            uint16      filterArgIndex;
            /** The checksum for this object (SHA-256) */
            uint8       checksum[32];

            /** Check correctness of this information for testing purpose */
            bool isCorrect(const uint64 fileSize, const uint64 fileOffset = 0) const { return header.isCorrect(fileSize, fileOffset) && fileSize >= getSize() + fileOffset; }
            /** Get the structure size (as some have optional fields) */
            static uint64 getSize() { return sizeof(Multichunk); }
            /** Load the structure from the given memory pointer */
            bool load(const uint8 * ptr, const uint64 size) { if (size <= getSize()) return false; memcpy(this, ptr, getSize()); return true; }
            /** Write the structure to the given memory pointer */
            void write(uint8 * ptr) { memcpy(ptr, this, getSize()); }
            /** Get the file base name for this multichunk */
            String getFileName() const;

            Multichunk(const uint16 UID = 0) : header(DataHeader::Multichunk, (uint32)getSize()), UID(UID), listID(0), filterArgIndex(0) { memset(checksum, 0, ArrSz(checksum)); }
        };

        /** The multichunks (it's a hash table of Multichunks which follow each other in the file). The size of this array is stored in the catalog */
        typedef Container::HashTable<Multichunk, uint16> Multichunks;
        /** The multichunks from the previous revisions */
        typedef Container::HashTable<Multichunk, uint16, Container::NoHashKey<uint16>, Container::NoDeletion<Multichunk> > MultichunksRO;

        /** The filter arguments. Usually, there's only one of them in the index file */
        struct FilterArguments
        {
            /** The block header */
            DataHeader header;
            /** The filter arguments */
            Strings::StringArray arguments;
            /** Check if this list was modified */
            bool modified;

            /** Check correctness of this information for testing purpose */
            bool isCorrect(const uint64 fileSize, const uint64 fileOffset = 0) const { return header.isCorrect(fileSize, fileOffset) && fileSize >= getSize() + fileOffset; }
            /** Get the structure size (as some have optional fields) */
            uint64 getSize() const { return sizeof(header) + ((arguments.Join().getLength()+1 + 3) & ~3); } // Need to round up to 4-bytes limit
            /** Load the structure from the given memory pointer */
            bool load(const uint8 * ptr, const uint64 size)
            {
                if (size <= sizeof(header)) return false; // Check if we can read the header
                memcpy(this, ptr, sizeof(header));
                if (size < header.getSize()) return false;

                Strings::FastString args((const char*)(ptr + sizeof(header)), (int)header.getSize());
                arguments.Clear();
                arguments.appendLines((const char*)args, "\n");
                modified = false;
                return true;
            }
            /** Write the structure to the given memory pointer */
            void write(uint8 * ptr)
            {
                header.setSize(getSize());
                memcpy(ptr, this, sizeof(header));
                const Strings::FastString & res = arguments.Join();
                memcpy(ptr+sizeof(header), (const char*)res, (res.getLength()+1 + 3) & ~3);
            }
            /** Append an argument to the filters */
            uint16 appendArgument(const String & argument) { modified = true; return (uint16)arguments.appendIfNotPresent(argument.Trimmed()); }
            /** Get the index for a given argument or arguments.getSize() if not found */
            uint16 getArgumentIndex(const String & argument) { return arguments.indexOf(argument); }
            /** Get the argument for the given index */
            const String & getArgument(const uint16 index) { return arguments[(size_t)index]; }
            /** Clear the arguments */
            void Reset() { modified = false; arguments.Clear(); }

            FilterArguments() : header(DataHeader::FilterArgument), modified(false) { }
        };

        /** The metadata about this backup */
        struct MetaData
        {
            /** The block header */
            DataHeader header;
            /** The initial backup path will be at position 0 in the array */
            Strings::StringArray info;
            /** Check if this object was modified */
            bool modified;
            /** Check correctness of this information for testing purpose */
            bool isCorrect(const uint64 fileSize, const uint64 fileOffset = 0) const { return header.isCorrect(fileSize, fileOffset) && fileSize >= getSize() + fileOffset; }
            /** Get the structure size (as some have optional fields) */
            uint64 getSize() const { return sizeof(header) + ((info.Join().getLength() + 3) & ~3); } // Need to round up to 4-bytes limit
            /** Get the backup path (first item in array) */
            const Strings::FastString & getBackupPath() const { return info[0]; }
            /** Append a line to the metadata array */
            void Append(const String & line) { modified = true; info.Append(line); }
            /** Load the structure from the given memory pointer */
            bool load(const uint8 * ptr, const uint64 size)
            {
                if (size <= sizeof(header)) return false; // Check if we can read the header
                memcpy(this, ptr, sizeof(header));
                if (size < header.getSize()) return false;

                Strings::FastString args((const char*)(ptr + sizeof(header)), (int)header.getSize());
                info.Clear();
                info.appendLines(args, "\n");
                modified = false;
                return true;
            }
            /** Write the structure to the given memory pointer */
            void write(uint8 * ptr)
            {
                header.setSize(getSize());
                memcpy(ptr, this, sizeof(header));
                const Strings::FastString & res = info.Join();
                memcpy(ptr+sizeof(header), (const char*)res, (res.getLength() + 3) & ~3);
            }
            /** Find a metadata that starts with the given key */
            String findKey(const String & key) const { for (size_t i = 0; i < info.getSize(); i++) if (info[i].upToFirst(":") == key) return info[i]; return ""; }
            /** Reset a metadata object */
            void Reset() { modified = false; info.Clear(); }

            MetaData() : header(DataHeader::Metadata), modified(false) { }
        };

        /** The file tree.
            The file tree is also stored as an array, but unlike the database format, we store the complete tree for each revision to avoid
            having to consolidate the file tree
            So, for a backup tree of 100k files, each file consumes:
              - 4 bytes for UID
              - 4 bytes for parent UID
              - 4 bytes (chunk lists' UID)
              - ~60 bytes (around for file's metadata)
              - ~22 bytes (for file's base name)
              = 94 bytes per file on average => 9.4MB of data for 100k files
            Because each file's record is variable size, and we want to avoid storing useless "length" field.
            Metadata size is known while parsing starts on the metadata field, so this does not requires any additional length storage.
            File's name is zero-char terminated. The last entry in a File tree block might get padded with zero so it ends on a 32 bits boundary */
        struct FileTree
        {
            /** The block header */
            DataHeader header;
            /** The current revision for this file tree */
            uint32 revision;
            /** The array of file items */
            struct Item
            {
                /** This structure can be pinned to memory directly */
                struct Fixed
                {
                    /** The parent index in the array + 1 (so it's 0 for no parent) */
                    uint32      parentID;
                    /** The chunk list identifier for this file */
                    uint32      chunkListID;
                    /** The size in bytes for the metadata field */
                    uint16      metadataSize;
                    /** The size in bytes for the file's base name */
                    uint16      baseNameSize;

                    Fixed() : parentID(0), chunkListID(0), metadataSize(0), baseNameSize(0) {}
                } * fixed;
                /** The metadata buffer */
                uint8 *     metaData;
                /** The file base name buffer */
                uint8 *     baseName;
                /** Whether this file tree is read only */
                bool        readOnly;


                /** Get the structure size (as some have optional fields) */
                uint64 getSize() const { return !fixed ? sizeof(Fixed) : ((sizeof(Fixed) + fixed->metadataSize + fixed->baseNameSize + 3) & ~3); } // Need to round up to 4-bytes limit
                /** Set the metadata buffer */
                Item & setParentID(const uint32 ID) { if (!readOnly && fixed) fixed->parentID = ID; return *this; }
                /** Set the metadata for this item */
                Item & setMetaData(const uint8 * buffer, const uint16 size) { if (!readOnly && fixed) { fixed->metadataSize = size; delete0(metaData); metaData = new uint8[size]; memcpy(metaData, buffer, size); } return *this; }
                /** Set the chunk list ID */
                Item & setChunkListID(const uint32 ID) { if (!readOnly && fixed) fixed->chunkListID = ID; return *this; }
                /** Set the file name for this item.
                    This methods accepts a complete path, and will only extract the base name from it */
                Item & setBaseName(const String & base)
                {
                    if (!readOnly && fixed) { fixed->baseNameSize = base.getLength(); delete0(baseName); baseName = new uint8[base.getLength()]; memcpy(baseName, (const char*)base, base.getLength()); }
                    return *this;
                }
                /** Get the parent ID or 0 if none */
                uint32 getParentID() const { return fixed ? fixed->parentID : 0; }
                /** Get the basename for this item */
                String getBaseName() const { if (!fixed || !baseName) return ""; return String(baseName, fixed->baseNameSize); }
                /** Get the file's metadata (it's expanded here) */
                String getMetaData() const { if (!fixed || !metaData) return ""; return File::Info::expandMetaData(metaData, fixed->metadataSize); }
                /** Get the chunk list ID for this file (or 0 on error) */
                uint32 getChunkListID() const { return fixed ? fixed->chunkListID : 0; }
                /** Check if the base name for a file match this one */
                bool checkBaseName(const String & base) const { if (!base && fixed && !fixed->parentID) return true; const String & tmp = base.normalizedPath(PathSeparator[0], false); return baseName && fixed && tmp.getLength() == fixed->baseNameSize && memcmp(baseName, (const char*)tmp, tmp.getLength()) == 0; }
                /** Create a valid structure */
                Item & renew() { if (!readOnly) { delete0(fixed); fixed = new Fixed; } return *this; }
                /** Create a new item from scratch that must be filled yourself */
                static inline Item & createNew(const bool readOnly) { return *new Item(readOnly); }
                /** Load the structure from the given memory pointer */
                bool load(const uint8 * ptr, const uint64 size)
                {
                    if (!readOnly)
                    {
                        if (size <= sizeof(Fixed)) return false; // Check if we can read the header
                        delete0(fixed); fixed = new Fixed;
                        memcpy(fixed, ptr, sizeof(Fixed));
                        if (size < sizeof(Fixed) + fixed->metadataSize + fixed->baseNameSize) return false;
                        delete0(metaData); metaData = new uint8[fixed->metadataSize];
                        memcpy(metaData, ptr + sizeof(Fixed), fixed->metadataSize);
                        delete0(baseName); baseName = new uint8[fixed->baseNameSize];
                        memcpy(baseName, ptr + sizeof(Fixed) + fixed->metadataSize, fixed->baseNameSize);
                    }
                    else
                    {   // Should place us upon the right memory area then check and set our internal pointer
                        fixed = (Fixed*)ptr;
                        if (size < sizeof(Fixed) + fixed->metadataSize + fixed->baseNameSize) return false;
                        metaData = (uint8*)(ptr + sizeof(Fixed));
                        baseName = (uint8*)(ptr + sizeof(Fixed) + fixed->metadataSize);
                    }
                    return true;
                }
                /** Write the structure to the given memory pointer */
                void write(uint8 * ptr)
                {
                    memcpy(ptr, fixed, sizeof(Fixed));
                    memcpy(ptr + sizeof(Fixed), metaData, fixed->metadataSize);
                    memcpy(ptr + sizeof(Fixed) + fixed->metadataSize, baseName, fixed->baseNameSize);
                }

                bool operator != (const Item & other)
                {
                    return (fixed && !other.fixed) || (!fixed && other.fixed)
                           || memcmp(fixed, other.fixed, sizeof(*fixed)) != 0 || memcmp(metaData, other.metaData, fixed->metadataSize) != 0 || memcmp(baseName, other.baseName, fixed->baseNameSize) != 0;
                }

                Item(const FileTree & parent) : fixed(0), metaData(0), baseName(0), readOnly(parent.readOnly) {}
                /** This should be used to build a root node only */
                Item(const bool readOnly) : fixed(readOnly ?  0 : new Fixed), metaData(0), baseName(0), readOnly(readOnly) { }
                ~Item() { if (!readOnly) { delete0(fixed); delete0(metaData); delete0(baseName); } }
            };
            /** The items in the tree */
            Container::NotConstructible<Item>::IndexList items;
            /** Whether this file tree is modifiable */
            const bool                         readOnly;


            /** Find the index for an item.
                You should avoid this method as it's O(N) */
            uint32 findItem(Item & item) const { return (uint32)items.indexOfMatching(item); }
            /** Find the index for an item, fast.
                This only works if the item is from this array only. */
            uint32 findItemFast(Item & item) const { const Item * cur = &item, * beg = &items[0]; if (cur >= beg && cur < beg + items.getSize()) return (cur - beg); return (uint32)items.getSize(); }
            /** Find the index for an item, based on it's path.
                This method complexity is, at worst, O(N) (for the case '/a/a/a/a/a/a/a/a/a/a'), but on average O(log N) since only the matching nodes are checked */
            uint32 findItem(const String & path) const
            {
                if (!path) return 0; // The root item is always at position 0 (even if no root item yet ;-)
                // First we need to split the path in segment and check in reverse order
                Strings::StringArray segments(path, PathSeparator);

                for (size_t i = items.getSize(); i; i--)
                {
                    if (items[i-1].checkBaseName(segments[segments.getSize() - 1]))
                    {
                        // We've found the item, so let's walk up the file tree to ensure every segment is matching
                        size_t h = i-1, s = segments.getSize() - 1;
                        while (s && (size_t)(items[h].fixed->parentID - 1) < items.getSize() && items[items[h].fixed->parentID - 1].checkBaseName(segments[s - 1]))
                        {
                            h = items[h].fixed->parentID - 1;
                            s--;
                        }
                        // Found ?
                        if (s == 0 && items[h].fixed->parentID == 0) return i-1;
                    }
                }
                return (uint32)items.getSize();
            }
            /** Get the item for the given index.
                @warning index validity is not checked */
            Item * getItem(const uint32 index) const { return items.getElementAtUncheckedPosition(index); }
            /** Get the NotFound value for any find method above */
            uint32 notFound() const { return (uint32)items.getSize(); }
            /** Get the complete path for the given item index */
            String getItemFullPath(const uint32 index) const
            {
                String ret;
                if (index >= (uint32)items.getSize()) return ret;
                ret = items[index].getBaseName();
                uint32 p = items[index].getParentID();
                while (p) { ret = items[p-1].getBaseName() + PathSeparator + ret; p = items[p-1].getParentID(); }
                return ret;
            }
            /** Check correctness of this information for testing purpose */
            bool isCorrect(const uint64 fileSize, const uint64 fileOffset = 0) const { return header.isCorrect(fileSize, fileOffset) && fileSize >= getSize() + fileOffset; }
            /** Get the structure size (as some have optional fields) */
            uint64 getSize() const
            {
                uint64 size = sizeof(header) + sizeof(revision) + sizeof(uint32); // For count, even if not stored here
                for (size_t i = 0; i < items.getSize(); i++) size += items[i].getSize();
                return size;
            }
            /** Append an item to the items list for this revision. */
            void appendItem(Item * item) { items.Append(item); }
            /** Load the structure from the given memory pointer */
            bool load(const uint8 * ptr, const uint64 size)
            {
                if (size <= sizeof(header) + sizeof(revision)) return false; // Check if we can read the header
                memcpy(this, ptr, sizeof(header) + sizeof(revision));
                if (size < header.getSize()) return false;
                items.Clear();
                uint64 offset = sizeof(header) + sizeof(revision);
                uint32 count = 0;
                memcpy(&count, ptr + offset, sizeof(count));
                offset += 4;
                while (count--)
                {
                    Item * item = new Item(*this);
                    if (!item->load(ptr + offset, size - offset)) return false;
                    items.Append(item);
                    offset += item->getSize();
                }
                return true;
            }
            /** Write the structure to the given memory pointer */
            void write(uint8 * ptr)
            {
                header.setSize(getSize());
                uint64 offset = sizeof(header) + sizeof(revision);
                memcpy(ptr, this, offset);
                uint32 count = (uint32)items.getSize();
                memcpy(ptr + offset, &count, sizeof(count)); offset += sizeof(count);
                for (size_t i = 0; i < items.getSize(); i++)
                {
                    items[i].write(ptr + offset);
                    offset += items[i].getSize();
                }
            }
            /** Clear this file tree */
            inline void Clear() { items.Clear(); }

            FileTree(const uint32 revision = 0, const bool readOnly = true) : header(DataHeader::FileTree), revision(revision), readOnly(readOnly) {}
        };

        /** The main file header. */
        struct MainHeader
        {
            /** The magic number */
            union { uint32 number; char text[4]; } magic;
            /** The file version and state */
            uint32 version;
            /** The catalog offset */
            Offset catalogOffset;
            /** The ciphered master key */
            uint8 cipheredMasterKey[108];

            /** Assert the file is valid */
            bool isSupportedFormat() const { return memcmp(magic.text, "Frst", 4) == 0 && version == 2; }
            /** Check correctness of this information for testing purpose */
            bool isCorrect(const uint64 fileSize, const uint64 fileOffset = 0) const { return isSupportedFormat() && catalogOffset.fileOffset() <= (fileSize - sizeof(Catalog)) && !isZero(cipheredMasterKey); }
            /** Get the structure size (as some have optional fields) */
            static uint64 getSize() { return sizeof(MainHeader); }
            /** Load the structure from the given memory pointer */
            void load(const uint8 * ptr) { memcpy(this, ptr, getSize()); }
            /** Write the structure to the given memory pointer */
            void write(uint8 * ptr) { memcpy(ptr, this, getSize()); }

            /** Default construction */
            MainHeader() : version(2) { memcpy(magic.text, "Frst", 4); memset(cipheredMasterKey, 0, ArrSz(cipheredMasterKey)); }
        };

#pragma pack(pop)

        /** The index file helper class.
            This class provided everything method required to deal with the file format being used */
        class IndexFile
        {
            // Members
        private:
            /** The file catalog (if any) */
            Utils::OwnPtr<Catalog>      catalog;
            /** The file header (if any) */
            Utils::OwnPtr<MainHeader>    header;
            /** The consolidated chunk array */
            Chunks          consolidated;
            /** The local chunk array */
            Chunks          local;
            /** The maximum chunk id found */
            uint32          maxChunkID;
            /** Was the file opened as read only ? */
            bool            readOnly;

            /** The chunk list for previous session */
            ChunkLists      chunkListRO;
            /** The chunk list for this session */
            ChunkLists      chunkList;
            /** The maximum chunk list UID */
            uint32          maxChunkListID;
            /** The multichunk list for this session */
            Multichunks     multichunks;
            /** The multichunks list for previous sessions */
            MultichunksRO   multichunksRO;
            /** The maximum multichunk UID */
            uint16          maxMultichunkID;
            /** The filters arguments */
            FilterArguments arguments;
            /** The metadata */
            MetaData        metadata;

            /** The current file tree */
            FileTree        fileTree;
            /** The file tree for the previous revision */
            FileTree        fileTreeRO;

            /** The memory mapped file */
            Utils::ScopePtr<Stream::MemoryMappedFileStream>      file;

            // Interface
        public:
            // Read-only first
            /** Read the file for every structure loading. */
            String readFile(const String & filePath, const bool readWrite = false);


            /** Get the chunks' consolidated array. */
            Chunks & getTotalChunks() { return consolidated; }
            /** Get the chunk list by ID */
            ChunkList * getChunkList(const uint32 ID) { ChunkList * l = chunkListRO.getValue(ID); if (l) return l; return chunkList.getValue(ID); }
            /** Get the multichunk by ID */
            Multichunk * getMultichunk(const uint16 ID) { Multichunk * m = multichunksRO.getValue(ID); if (m) return m; return multichunks.getValue(ID); }
            /** Get the file tree */
            Utils::OwnPtr<FileTree> getFileTree(const uint32 revision);
            /** Get the current revision */
            uint32 getCurrentRevision() const { return readOnly ? fileTreeRO.revision : fileTree.revision; }
            /** Get the filter arguments */
            FilterArguments & getFilterArguments() { return arguments; }
            /** Get the metadata */
            MetaData & getMetaData() { return metadata; }
            /** Get the ciphered master key */
            Utils::MemoryBlock getCipheredMasterKey() const { return Utils::MemoryBlock(header ? header->cipheredMasterKey : 0, header ? ArrSz(header->cipheredMasterKey) : 0); }
            /** Find a chunk ID from its checksum (return -1 if not found) */
            uint32 findChunk(Chunk & chunk) const { return consolidated.findChunk(chunk); }
            /** Search the chunks by UID */
            const Chunk * findChunk(const uint32 uid) const;

            /** Get the current catalog */
            const Catalog * getCatalog() const { return catalog; }

            /** Allocate a multichunk ID */
            uint16 allocateMultichunkID() const { return maxMultichunkID + 1; }
            /** Allocate a chunklist ID */
            uint32 allocateChunkListID() const { return maxChunkListID + 1; }
            /** Allocate a chunk ID */
            uint32 allocateChunkID() const { return maxChunkID + 1; }

            /** Map a structure at the given position */
            template <typename T>
            bool Map(const T *& s, const Offset & offset) const { if (offset.fileOffset() < file->fullSize()) { s = (T*)(file->getBuffer() + offset.fileOffset()); return true; } return false; }
            /** Load a structure that can't be mapped. */
            template <typename T>
            bool Load(T & s, const Offset & offset) const { return s.load(file->getBuffer() + offset.fileOffset(), file->fullSize() - offset.fileOffset()); }

            // Write operations
            /** Append a chunk to the internal chunk array (common and private version) */
            bool appendChunk(Chunk & chunk);
            /** Append a multichunk to this file (and its chunk list)
                @param mchunk   A pointer to a new allocated multichunk that is owned
                @param list     A pointer to a new allocated list that is owned
                @return true on success */
            bool appendMultichunk(Multichunk * mchunk, ChunkList * list);
            /** Append a file item
                @param item     A pointer to a new allocated file item
                @param list     A pointer to a new allocated list that's owned
                @return true on success */
            bool appendFileItem(FileTree::Item * item, ChunkList * list);


            /** Start a new revision for this backup file */
            bool startNewRevision();
            /** Create a new file from scratch.
                This is not done at construction, because we must be able to tell the difference
                between opening and creating (non-existent file is not enough to distinguish)
                @return A empty string on success, or a translated error message on error */
            String createNew(const String & filePath, const Utils::MemoryBlock & cipheredMasterKey, const String & backupPath);

            // Generic operations
            /** Close the file. */
            String close();
        };
    }

#if KeepPreviousVersionFormat == 1
    /** The database model we are following. */
    namespace DatabaseModel
    {
        /** The Index file metadata part */
        struct IndexDescription : public Database::Table<IndexDescription>
        {
            BeginFieldDeclarationDelayEx(IndexDescription)
                DeclareFieldEx(Version, Database::NotNullString, "1.0");
                DeclareField(CipheredMasterKey, String);
                DeclareField(InitialBackupPath, String);
                DeclareField(CurrentRevisionID, unsigned int);
                DeclareField(Description, String);
            EndFieldDeclaration
        };

        /** A chunk declaration.
            We don't use a blob for the checksum, because it's easier to debug with
            a plain old hexadecimal string and the difference is size does not justify the cost */
        struct Chunk : public Database::Table<Chunk>
        {
            BeginFieldDeclarationEx(Chunk)
                DeclareField(ID, Database::LongIndex);
                DeclareFieldWithIndex(Checksum, String, "", true);
                DeclareField(Size, unsigned int);
            EndFieldDeclaration
        };

        /** A logical list of chunks.
            Because chunks will be reused in different files, the files links to this list */
        struct ChunkList : public Database::Table<ChunkList>
        {
            BeginFieldDeclarationDelayEx(ChunkList)
                DeclareField(ID, uint64);
                DeclareFieldWithIndex(ChunkID, uint64, "0", false);
                DeclareField(Offset, uint64);
                DeclareField(Type, int); // This is used to avoid a useless query later on, 0 for file, 1 for multichunk
            EndFieldDeclaration
        };

        /** The multichunk declaration (this is similar to a chunklist,
            but stores the filtering information, and actual location in the remote folder of the data)  */
        struct MultiChunk : public Database::Table<MultiChunk>
        {
            BeginFieldDeclarationEx(MultiChunk)
                DeclareField(ID, Database::Index);
                DeclareFieldWithIndex(ChunkListID, uint64, "0", true);
                DeclareField(FilterListID, unsigned int);
                DeclareField(FilterArgument, String);
                DeclareField(Path, String);
            EndFieldDeclaration
        };

        /** A file entry (this maps files to chunks) - deprecated */
        struct File : public Database::Table<File>
        {
            BeginFieldDeclarationEx(File)
                DeclareField(ID, Database::Index);
                DeclareField(ChunkListID, uint64);
                DeclareField(ParentDirectoryID, unsigned int);
                DeclareField(Metadata, String);
                DeclareFieldEx(Revision, unsigned int, "0");
                DeclareField(Path, Database::NotNullString);
            EndFieldDeclaration
        };

        /** A directory entry - deprecated */
        struct Directory : public Database::Table<Directory>
        {
            BeginFieldDeclarationEx(Directory)
                DeclareField(ID, Database::Index);
                DeclareField(Path, Database::NotNullString);
                DeclareField(ParentDirectoryID, unsigned int);
                DeclareField(Metadata, String);
                DeclareFieldEx(Revision, unsigned int, "0");
            EndFieldDeclaration
        };

        /** A file or directory entry (this maps files to chunks).
            This deprecates the previous Directory & File table that were only growing in size.

            Typically, this tracks both the file type (directory or file) and the state (modified or deleted).

            For example, here's a complete description of successive operations in backups:
            @verbatim
            Initial file tree:
              root
               |--- file1
               |--- file2

            this will lead on first backup these 3 entries:
               - root (type d, rev 1, state m)
               - file1 (type f, rev 1, state m)
               - file2 (type f, rev 1, state m)
            @endverbatim

            Someone adds some files to the root folder:
            @verbatim
            New file tree:
              root
               |--- file1
               |--- file2
               |--- subdir
                      |--- file3

            this will lead on next backup these entries:
               - root (type d, rev 1, state m)
               - file1 (type f, rev 1, state m)
               - file2 (type f, rev 1, state m)

               - root (type d, rev 2, state m)    Due to change in modification time
               - subdir (type d, rev 2, state m)
               - file3 (type f, rev 2, state m)
            @endverbatim

            Then, someone deletes a file:
            @verbatim
            New file tree:
              root
               |--- file1
               |--- subdir
                      |--- file3

            this will lead on next backup these entries:
               - root (type d, rev 1, state m)
               - file1 (type f, rev 1, state m)
               - file2 (type f, rev 1, state m)

               - root (type d, rev 2, state m)
               - subdir (type d, rev 2, state m)
               - file3 (type f, rev 2, state m)

               - root (type d, rev 3, state m)    Due to change in modification time
               - file2 (type f, rev 3, state d)
            @endverbatim

            Then someone deletes a dir:
            @verbatim
            New file tree:
              root
               |--- file1

            this will lead on next backup these entries:
               - root (type d, rev 1, state m)
               - file1 (type f, rev 1, state m)
               - file2 (type f, rev 1, state m)

               - root (type d, rev 2, state m)
               - subdir (type d, rev 2, state m)
               - file3 (type f, rev 2, state m)

               - root (type d, rev 3, state m)
               - file2 (type f, rev 3, state d)

               - root (type d, rev 4, state m)    Due to change in modification time
               - subdir (type d, rev 4, state d)
               - file3 (type f, rev 4, state d)
            @endverbatim

            Then, after some revisions, add another file with the same name as the previous one:
            @verbatim
            New file tree:
              root
               |--- file1
               |--- subdir
                      |--- file3

            this will lead on next backup these entries:
               - root (type d, rev 1, state m)
               - file1 (type f, rev 1, state m)
               - file2 (type f, rev 1, state m)

               - root (type d, rev 2, state m)
               - subdir (type d, rev 2, state m)
               - file3 (type f, rev 2, state m)

               - root (type d, rev 3, state m)
               - file2 (type f, rev 3, state d)

               - root (type d, rev 4, state m)
               - subdir (type d, rev 4, state d)
               - file3 (type f, rev 4, state d)
               [...]
               - root (type d, rev 6, state m)    Due to change in modification time
               - subdir (type d, rev 6, state m)
               - file3 (type f, rev 6, state m)
            @endverbatim
        */
        struct Entry : public Database::Table<Entry>
        {
            BeginFieldDeclarationEx(Entry)
                DeclareField(ID, Database::Index);
                DeclareField(ChunkListID, uint64);
                DeclareField(ParentEntryID, unsigned int);
                DeclareField(Metadata, String);
                DeclareFieldEx(Revision, unsigned int, "0");
                DeclareFieldWithIndex(Path, Database::NotNullString, "", false);
                DeclareFieldEx(Type, unsigned int, "0");   // 0 for File, 1 for Directory
                DeclareFieldEx(State, unsigned int, "0");  // 0 for New/Modified, 1 for Deleted
            EndFieldDeclaration
        };

        /** The revision iteration.
            Each backup increment the revision number. If a file is modified in a revision,
            the previous revision is not deleted (unless pruning is requested).
            If a file is not modified in a revision, its revision number is not modified */
        struct Revision : public Database::Table<Revision>
        {
            BeginFieldDeclarationEx(Revision)
                DeclareField(ID, Database::Index);
                DeclareField(TimeSinceEpoch, uint64);
                DeclareField(RevisionTime, String);
                DeclareFieldEx(FileCount, unsigned int, "0");
                DeclareFieldEx(DirCount, unsigned int, "0");
                DeclareFieldEx(InitialSize, uint64, "0");
                DeclareFieldEx(BackupSize, uint64, "0");
            EndFieldDeclaration
        };

        /** Declare the database format we are using */
        struct FrostDB : public Database::Base<FrostDB>
        {
            BeginTableDeclaration(FrostDB)
                DeclareTable(IndexDescription)
                DeclareTable(Revision)
                DeclareTable(Entry)
                DeclareTable(MultiChunk)
                DeclareTable(ChunkList)
                DeclareTable(Chunk)
            EndTableDeclarationRegister(FrostDB)
        };

        /** The database complete URL to use */
        extern String databaseURL;

        BeginDatabaseConnection
            DeclareDatabaseWithComplexDBNameDynURL(FrostDB, "FrostDB", databaseURL, DEFAULT_INDEX)
        EndDatabaseConnection
    }
#endif

    // The backup functions
    /** Backup the given folder.
        @param folderToBackup   This the root of the folder to backup. All files will be saved in the backup relative to this root folder
        @param backupTo         The folder to store the multichunk into.
        @param revisionID       The current backup revision identifier
        @param callback         The progress callback that's called at regular interval
        @param strategy         The backing up strategy ('Slow' means reopening last multichunk to append to it thus creating less files in backup folder, 'Fast' is default)
        @return A string describing the error, or an empty string on success */
    String backupFolder(const String & folderToBackup, const String & backupTo, const unsigned int revisionID, ProgressCallback & callback, const PurgeStrategy strategy = Fast);
    /** List available backups.
        @param folderToBackup   This the root of the folder to backup. All files will be saved in the backup relative to this root folder
        @param backupTo         The folder to store the multichunk into.
        @param revisionID       The current backup revision identifier
        @param withList         If true, the file list is display for this revision
        @return The number of revisions listed */
    unsigned int listBackups(const ::Time::Time & startTime = ::Time::Epoch, const ::Time::Time & endTime = ::Time::MaxTime, const bool withList = false);
    /** Purge a backup from the given folder to space some space.
        @param chunkFolder      This the root of the folder where the chunk were saved
        @param strategy         The strategy to follow while globbering space to purge
        @param upToRevision     The last revision to clean up (inclusive)
        @return A string describing the error, or an empty string on success */
    String purgeBackup(const String & chunkFolder, ProgressCallback & callback, const PurgeStrategy strategy = Fast, const unsigned int upToRevision = 0);
    /* Restore a backup to the given folder.
        @param folderToRestore  This the root of the folder to restore the files into. All files will be restored from their relative position in the backup to this root folder
        @param restoreFrom      The folder to load the multichunk from.
        @param revisionID       The current backup revision identifier
        @param callback         The progress callback that's called at regular interval
        @return A string describing the error, or an empty string on success */
    String restoreBackup(const String & folderToRestore, const String & restoreFrom, const unsigned int revisionID, ProgressCallback & callback, const size_t maxCacheSize = 64*1024*1024);
}
