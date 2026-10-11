package nxs

import "errors"

var errObjectNotImplemented = errors.New("nxs: standalone objects not implemented")

// ObjectEncoder emits a single NYXO object for a fixed schema, independent of
// a file preamble or tail-index.
type ObjectEncoder struct{}

// NewObjectEncoder returns an encoder for objects with the given keys and
// per-slot sigils.
func NewObjectEncoder(keys []string, sigils []byte) *ObjectEncoder { return &ObjectEncoder{} }

func (e *ObjectEncoder) Reset()                           {}
func (e *ObjectEncoder) WriteI64(slot int, v int64)       {}
func (e *ObjectEncoder) WriteF64(slot int, v float64)     {}
func (e *ObjectEncoder) WriteBool(slot int, v bool)       {}
func (e *ObjectEncoder) WriteStr(slot int, v string)      {}
func (e *ObjectEncoder) WriteBytes(slot int, v []byte)    {}
func (e *ObjectEncoder) WriteTime(slot int, unixNs int64) {}
func (e *ObjectEncoder) WriteNull(slot int)               {}

func (e *ObjectEncoder) Bytes() ([]byte, error) { return nil, errObjectNotImplemented }

// DecodeObject returns a read view over one standalone NYXO object.
func DecodeObject(keys []string, sigils []byte, obj []byte) (*Object, error) {
	return nil, errObjectNotImplemented
}

func (o *Object) GetBytes(key string) ([]byte, bool) { return nil, false }
func (o *Object) GetTime(key string) (int64, bool)   { return 0, false }
func (o *Object) Has(key string) bool                { return false }
